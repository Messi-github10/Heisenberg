#include "EditorSession.hpp"

#include <QDir>
#include <QFileInfo>
#include <QUuid>

namespace heisenberg::ui {
EditorSession::EditorSession(QObject* parent) : QObject(parent),
    token_(QUuid::createUuid().toString(QUuid::WithoutBraces)) {}

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
    token_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    syncDirty();
    emit projectChanged();
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

EditResult EditorSession::placeMedia(const QString& mediaRef, qint64 frame, const QString& targetTrackId) {
    auto result = project_.editor().placeClip(mediaRef.toUtf8().toStdString(), frame, -1, 0,
                                            targetTrackId.toUtf8().toStdString());
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
