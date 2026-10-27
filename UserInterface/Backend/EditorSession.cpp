#include "EditorSession.hpp"

#include <QDir>
#include <QFileInfo>
#include <QUuid>
#include <QStandardPaths>
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <limits>
#include <Compositing/Grade/LutLibrary.hpp>

namespace heisenberg::ui {
EditorSession::EditorSession(QObject* parent) : QObject(parent),
    token_(QUuid::createUuid().toString(QUuid::WithoutBraces)) {
    const auto root = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/LUTs";
    QDir().mkpath(root);
    grade::LutLibrary::setDefaultRoot(std::filesystem::u8path(QDir::fromNativeSeparators(root).toUtf8().toStdString()));
}

void EditorSession::syncDirty() {
    const bool dirty = project_.isDirty();
    if (dirty_ == dirty) return;
    dirty_ = dirty;
    emit dirtyChanged(dirty);
}

bool EditorSession::loadPackage(const QString& path, QString* error) {
    std::string reason;
    if (!project_.loadPackage(path.toUtf8().toStdString(), &reason)) {
        if (error) *error = QString::fromUtf8(reason.c_str());
        return false;
    }
    const auto markedRefs = sourceRanges_.keys();
    sourceRanges_.clear();
    token_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    syncDirty();
    emit projectChanged();
    for (const auto& ref : markedRefs) emit sourceRangeChanged(ref);
    emit mediaChanged();
    emit timelineChanged(0);
    return true;
}

bool EditorSession::savePackage(const QString& path, QString* error) {
    const auto previousRoot = project_.editor().projectDir();
    std::string reason;
    if (!project_.savePackage(path.toUtf8().toStdString(), &reason)) {
        if (error) *error = QString::fromUtf8(reason.c_str());
        return false;
    }
    syncDirty();
    if (previousRoot != project_.editor().projectDir()) emit mediaChanged();
    return true;
}

QStringList EditorSession::importFiles(const QStringList& paths) {
    QStringList errors;
    bool changed = false;
    for (const auto& path : paths) {
        const QFileInfo file(path);
        const QString absolute = file.canonicalFilePath();
        if (absolute.isEmpty() || !file.isFile()) {
            errors << tr("%1: file is unavailable").arg(file.fileName());
            continue;
        }
        bool duplicate = false;
        for (const auto& asset : project_.editor().assets()) {
            if (QFileInfo(QString::fromUtf8(asset.path.c_str())).canonicalFilePath()
                    .compare(absolute, Qt::CaseInsensitive) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;
        std::string reason;
        if (project_.editor().importMedia(QDir::fromNativeSeparators(absolute).toUtf8().toStdString(), &reason))
            changed = true;
        else errors << tr("%1: %2").arg(file.fileName(), QString::fromUtf8(reason.c_str()));
    }
    if (changed) {
        syncDirty();
        emit mediaChanged();
    }
    return errors;
}

SourceRange EditorSession::sourceRange(const QString& mediaRef) const {
    return sourceRanges_.value(mediaRef);
}

bool EditorSession::setSourceBoundary(const QString& mediaRef, double seconds, bool in) {
    const auto* media = project_.editor().manifest().find(mediaRef.toUtf8().toStdString());
    if (!media || !std::isfinite(media->duration) || media->duration <= 0.0 ||
        !std::isfinite(seconds) || seconds < 0.0 || seconds > media->duration) return false;
    auto range = sourceRange(mediaRef);
    if (in) range.inSeconds = seconds;
    else range.outSeconds = seconds;
    if (range.inSeconds.value_or(0.0) >= range.outSeconds.value_or(media->duration)) return false;
    if (range == sourceRange(mediaRef)) return true;
    sourceRanges_.insert(mediaRef, range);
    emit sourceRangeChanged(mediaRef);
    return true;
}

bool EditorSession::setSourceIn(const QString& mediaRef, double seconds) {
    return setSourceBoundary(mediaRef, seconds, true);
}

bool EditorSession::setSourceOut(const QString& mediaRef, double seconds) {
    return setSourceBoundary(mediaRef, seconds, false);
}

bool EditorSession::clearSourceRange(const QString& mediaRef) {
    if (!project_.editor().manifest().find(mediaRef.toUtf8().toStdString())) return false;
    if (sourceRanges_.remove(mediaRef)) emit sourceRangeChanged(mediaRef);
    return true;
}

bool EditorSession::sourcePlacementRange(const QString& mediaRef, qint64& trimStart,
                                          qint64& duration, QString* error) const {
    const auto fail = [error](const QString& reason) {
        if (error) *error = reason;
        return false;
    };
    const auto* media = project_.editor().manifest().find(mediaRef.toUtf8().toStdString());
    const auto* timeline = project_.editor().activeTimeline();
    if (!media || !timeline) return fail(tr("Media or active timeline was not found."));
    if (!std::isfinite(media->duration) || media->duration <= 0.0 ||
        !std::isfinite(timeline->fps) || timeline->fps <= 0.0)
        return fail(tr("Source duration and timeline frame rate must be positive."));
    const double sourceFrames = media->duration * timeline->fps;
    if (!std::isfinite(sourceFrames) ||
        sourceFrames >= static_cast<double>(std::numeric_limits<qint64>::max()))
        return fail(tr("Source duration cannot be represented in timeline frames."));
    const qint64 sourceDuration = std::llround(sourceFrames);
    const auto range = sourceRange(mediaRef);
    const double in = range.inSeconds.value_or(0.0);
    const double out = range.outSeconds.value_or(media->duration);
    if (!std::isfinite(in) || !std::isfinite(out) || in < 0.0 || out > media->duration || in >= out)
        return fail(tr("Source In must precede Out within the media duration."));
    SourcePlacementRange placement;
    placement.trimStartFrame = std::clamp<qint64>(std::llround(in * timeline->fps), 0, sourceDuration);
    const qint64 end = std::clamp<qint64>(std::llround(out * timeline->fps), 0, sourceDuration);
    if (end <= placement.trimStartFrame)
        return fail(tr("Source range contains no frames at the timeline frame rate."));
    placement.durationFrames = end - placement.trimStartFrame;
    trimStart = placement.trimStartFrame;
    duration = placement.durationFrames;
    if (error) error->clear();
    return true;
}

EditResult EditorSession::placeMedia(const QString& mediaRef, qint64 frame, const QString& targetTrackId) {
    qint64 trimStart = 0, duration = -1;
    QString error;
    if (!sourcePlacementRange(mediaRef, trimStart, duration, &error))
        return {EditStatus::Invalid, {}, error.toUtf8().toStdString()};
    auto result = project_.editor().placeClip(mediaRef.toUtf8().toStdString(), frame, duration, trimStart,
                                            targetTrackId.toUtf8().toStdString());
    if (result.changed()) {
        syncDirty();
        emit timelineChanged(frame);
    }
    return result;
}

EditResult EditorSession::insertMedia(const QString& mediaRef, qint64 frame, const QString& targetTrackId) {
    qint64 trimStart = 0, duration = -1;
    QString error;
    if (!sourcePlacementRange(mediaRef, trimStart, duration, &error))
        return {EditStatus::Invalid, {}, error.toUtf8().toStdString()};
    auto result = project_.editor().insertClip(mediaRef.toUtf8().toStdString(), frame, duration, trimStart,
                                             targetTrackId.toUtf8().toStdString());
    if (result.changed()) {
        syncDirty();
        emit timelineChanged(frame);
    }
    return result;
}

EditResult EditorSession::rippleDeleteClip(const QString& clipId) {
    const auto* timeline = project_.editor().activeTimeline();
    const auto* clip = timeline ? timeline->clipById(clipId.toUtf8().toStdString()) : nullptr;
    const qint64 frame = clip ? clip->startFrame : -1;
    auto result = project_.editor().rippleDeleteClip(clipId.toUtf8().toStdString());
    if (result.changed()) {
        syncDirty();
        emit timelineChanged(frame);
    }
    return result;
}

EditResult EditorSession::setClipAudio(const QString& clipId, double volume,
                                      qint64 fadeInFrames, qint64 fadeOutFrames) {
    auto result = project_.editor().setClipAudio(clipId.toUtf8().toStdString(), volume,
                                               fadeInFrames, fadeOutFrames);
    if (result.changed()) { syncDirty(); emit timelineChanged(-1); }
    return result;
}

EditResult EditorSession::setColorSettings(const ColorSettings& settings) {
    auto result = project_.editor().setColorSettings(settings);
    if (result.changed()) { syncDirty(); emit timelineChanged(-1); }
    return result;
}

EditResult EditorSession::setMediaInputColorSpace(const QString& mediaRef, const QString& space) {
    auto result = project_.editor().setMediaInputColorSpace(mediaRef.toUtf8().toStdString(), space.toUtf8().toStdString());
    if (result.changed()) { syncDirty(); emit timelineChanged(-1); }
    return result;
}

EditResult EditorSession::setClipEffects(const QString& clipId, std::optional<JsonValue> effects) {
    auto result = project_.editor().setClipEffects(clipId.toUtf8().toStdString(), std::move(effects));
    if (result.changed()) { syncDirty(); emit timelineChanged(-1); }
    return result;
}

EditResult EditorSession::beginClipEffectsTransaction(const QString& clipId) {
    return project_.editor().beginClipEffectsTransaction(clipId.toStdString());
}

EditResult EditorSession::updateClipEffectsTransaction(std::optional<JsonValue> effects) {
    auto result = project_.editor().updateClipEffectsTransaction(std::move(effects));
    if (result.changed()) emit timelineChanged(-1);
    return result;
}

EditResult EditorSession::commitClipEffectsTransaction() {
    auto result = project_.editor().commitClipEffectsTransaction();
    syncDirty();
    if (result.changed()) emit timelineChanged(-1);
    return result;
}

EditResult EditorSession::cancelClipEffectsTransaction() {
    auto result = project_.editor().cancelClipEffectsTransaction();
    syncDirty();
    if (result.changed()) emit timelineChanged(-1);
    return result;
}

bool EditorSession::importGradeLut(const QString& path, QString& id, QString& displayName, QString& error) {
    grade::LutLibrary library(grade::LutLibrary::configuredRoot());
    std::string reason;
    const auto asset = library.importFile(std::filesystem::u8path(QDir::fromNativeSeparators(path).toUtf8().toStdString()), &reason);
    if (!asset) { error = QString::fromStdString(reason); return false; }
    id = QString::fromStdString(asset->id);
    displayName = QString::fromStdString(asset->displayName);
    return true;
}

EditResult EditorSession::setTrackMuted(const QString& trackId, bool muted) {
    auto result = project_.editor().setTrackMuted(trackId.toUtf8().toStdString(), muted);
    if (result.changed()) { syncDirty(); emit timelineChanged(-1); }
    return result;
}

EditResult EditorSession::removeClips(const QStringList& clipIds) {
    std::vector<std::string> ids;
    ids.reserve(clipIds.size());
    for (const auto& id : clipIds) ids.push_back(id.toUtf8().toStdString());
    auto result = project_.editor().removeClips(ids);
    if (result.changed()) { syncDirty(); emit timelineChanged(-1); }
    return result;
}

EditResult EditorSession::moveClip(const QString& clipId, qint64 frame) {
    if (project_.editor().compatibility().level == CompatibilityLevel::Unsupported)
        return {EditStatus::Unsupported, {}, "active timeline has an unsupported track or clip layout"};
    auto result = project_.editor().moveClip(clipId.toUtf8().toStdString(), frame);
    if (result.changed()) {
        syncDirty();
        emit timelineChanged(frame);
    }
    return result;
}

EditResult EditorSession::trimClip(const QString& clipId, TrimEdge edge, qint64 deltaFrames) {
    if (project_.editor().compatibility().level == CompatibilityLevel::Unsupported)
        return {EditStatus::Unsupported, {}, "active timeline has an unsupported track or clip layout"};
    auto result = project_.editor().trimClip(clipId.toUtf8().toStdString(), edge, deltaFrames);
    if (result.changed()) {
        syncDirty();
        const auto* clip = project_.editor().activeTimeline()->clipById(clipId.toUtf8().toStdString());
        emit timelineChanged(edge == TrimEdge::Left ? clip->startFrame : clip->endFrame() - 1);
    }
    return result;
}

EditResult EditorSession::splitClip(const QString& clipId, qint64 frame) {
    if (project_.editor().compatibility().level == CompatibilityLevel::Unsupported)
        return {EditStatus::Unsupported, {}, "active timeline has an unsupported track or clip layout"};
    auto result = project_.editor().splitClip(clipId.toUtf8().toStdString(), frame);
    if (result.changed()) {
        syncDirty();
        emit timelineChanged(frame);
    }
    return result;
}

void EditorSession::undo() {
    if (project_.editor().undo()) { syncDirty(); emit timelineChanged(-1); }
}

void EditorSession::redo() {
    if (project_.editor().redo()) { syncDirty(); emit timelineChanged(-1); }
}
EditResult EditorSession::applyTimelineSettings(const TimelineSettings& settings) {
    ProjectSettings projectSettings(project_);
    auto result = projectSettings.applyTimelineSettings(settings);
    if (result.changed()) {
        syncDirty();
        emit projectChanged();
        emit timelineChanged(-1);
    }
    return result;
}
} // namespace heisenberg::ui
