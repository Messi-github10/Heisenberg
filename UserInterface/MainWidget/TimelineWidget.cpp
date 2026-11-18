#include "MainWidget/TimelineWidget.hpp"
#include "Backend/EditorSession.hpp"
#include "MainWidget/ColorGradeDialog.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QStringList>
#include <QWheelEvent>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QJsonDocument>
#include <QJsonObject>
#include <QApplication>
#include <QKeyEvent>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QPointer>
#include <QScopedValueRollback>
#include <QInputDialog>
#include <QMessageBox>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStyle>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace {
constexpr double kHeaderWidth = 160.0;
constexpr double kRulerHeight = 30.0;
constexpr double kNewVideoTrackStripHeight = 20.0;
constexpr double kDefaultTrackHeight = 44.0;
constexpr double kMinZoom = 0.05;
constexpr double kMaxZoom = 40.0;
constexpr double kTrimHandleWidth = 6.0;

const QColor kBase(30, 31, 35);
const QColor kSurface(30, 31, 35);
const QColor kRaised(38, 39, 44);
const QColor kProminent(48, 49, 55);
const QColor kBorder(255, 255, 255, 24);
const QColor kSubtleBorder(255, 255, 255, 15);
const QColor kPrimaryText(255, 255, 255, 246);
const QColor kSecondaryText(255, 255, 255, 210);
const QColor kTertiaryText(255, 255, 255, 165);
const QColor kMutedText(255, 255, 255, 110);
const QColor kPlayhead(255, 82, 77);

double boundedTrackHeight(double height) {
    if (!std::isfinite(height)) return kDefaultTrackHeight;
    return std::clamp(height, 32.0, 200.0);
}

qint64 availableTrimFrames(qint64 sourceBudget, double speed, qint64 maximum) {
    // Match the model's round(delta * speed), including fractional-speed restoration.
    qint64 low = 0, high = std::max<qint64>(0, maximum);
    while (low < high) {
        const qint64 middle = low + (high - low + 1) / 2;
        const double consumed = std::round(static_cast<double>(middle) * speed);
        if (std::isfinite(consumed) && consumed < static_cast<double>(std::numeric_limits<qint64>::max()) &&
            static_cast<qint64>(consumed) <= sourceBudget) low = middle;
        else high = middle - 1;
    }
    return low;
}
}

TimelineWidget::TimelineWidget(QWidget* parent)
    : QWidget(parent) {
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAcceptDrops(true);
    verticalScrollBar_ = new QScrollBar(Qt::Vertical, this);
    verticalScrollBar_->setObjectName("timelineVerticalScrollBar");
    verticalScrollBar_->hide();
    verticalScrollBar_->setSingleStep(static_cast<int>(kDefaultTrackHeight));
    connect(verticalScrollBar_, &QScrollBar::valueChanged, this, [this] {
        // Drag previews store IDs; resolve the lane again after scrolling.
        dropDestination_.reset();
        draggedMedia_.clear();
        if (clipMove_ && clipMove_->dragging)
            updateClipMove(clipMove_->lastPosition, clipMove_->modifiers);
        if (marquee_ && marquee_->dragging) updateMarquee(marquee_->lastPosition);
        update();
    });
}

void TimelineWidget::setSession(heisenberg::ui::EditorSession* session) {
    if (session_) disconnect(session_, nullptr, this, nullptr);
    cancelInteraction();
    selectedTrackId_.clear();
    videoTargetTrackId_.clear();
    audioTargetTrackId_.clear();
    verticalScrollBar_->setValue(0);
    setSelectedClipId({});
    session_ = session;
    if (!session_) { refreshProject(); return; }
    connect(session_, &heisenberg::ui::EditorSession::projectChanged, this, [this]() {
        cancelInteraction();
        selectedTrackId_.clear();
        videoTargetTrackId_.clear();
        audioTargetTrackId_.clear();
        verticalScrollBar_->setValue(0);
        setSelectedClipId({});
        hoveredClipId_.clear();
        currentFrame_ = 0;
        scrollOffset_ = 0.0;
        pixelsPerFrame_ = 4.0;
        const auto& project = session_->editor().project();
        if (const auto* active = timeline(); active && project.viewStates) {
            const auto state = project.viewStates->find(active->id);
            if (state != project.viewStates->end()) {
                pixelsPerFrame_ = state->second.zoomScale;
                scrollOffset_ = state->second.scrollOffsetX;
            }
        }
        refreshProject();
    });
    connect(session_, &heisenberg::ui::EditorSession::mediaChanged, this, &TimelineWidget::refreshProject);
    connect(session_, &heisenberg::ui::EditorSession::sourceRangeChanged, this, [this](const QString&) { update(); });
    connect(session_, &heisenberg::ui::EditorSession::timelineChanged, this, [this](qint64 frame) {
        cancelInteraction();
        refreshProject();
        if (frame >= 0) setCurrentFrame(frame);
    });
    refreshProject();
}

void TimelineWidget::refreshProject() {
    draggedMedia_.clear();
    dropDestination_.reset();
    mediaNames_.clear();
    missingMedia_.clear();
    if (session_) {
        for (const auto& asset : session_->editor().assets()) {
            mediaNames_.emplace(asset.id, asset.name);
            if (asset.missing) missingMedia_.insert(asset.id);
        }
    }
    playbackFrameCount_ = timeline() ? timeline()->totalFrames() : 0;
    if (std::any_of(selectedClipIds_.begin(), selectedClipIds_.end(), [this](const auto& id) {
            return !timeline() || !timeline()->clipById(id);
        })) setSelection(selectedClipIds_, selectedClipId_);
    currentFrame_ = std::clamp<qint64>(currentFrame_, 0, std::max<qint64>(0, contentFrameCount() - 1));
    if (!timeline() || !timeline()->trackById(selectedTrackId_)) selectedTrackId_.clear();
    if (!timeline() || !timeline()->trackById(videoTargetTrackId_)) videoTargetTrackId_.clear();
    if (!timeline() || !timeline()->trackById(audioTargetTrackId_)) audioTargetTrackId_.clear();
    updateVerticalScrollBar();
    clampView();
    updateGeometry();
    update();
}

void TimelineWidget::setFrameCount(qint64 frameCount) {
    playbackFrameCount_ = std::max<qint64>(0, frameCount);
    currentFrame_ = std::clamp<qint64>(currentFrame_, 0, std::max<qint64>(0, contentFrameCount() - 1));
    clampView();
    update();
}

void TimelineWidget::setCurrentFrame(qint64 frame) {
    if (scrubbing_ || (clipMove_ && clipMove_->dragging)) return;
    currentFrame_ = std::clamp<qint64>(frame, 0, std::max<qint64>(0, contentFrameCount() - 1));
    update();
}

bool TimelineWidget::cancelInteraction() {
    const bool active = clipMove_.has_value() || marquee_.has_value() || scrubbing_ ||
        !draggedMedia_.isEmpty() || dropDestination_.has_value();
    clipMove_.reset();
    marquee_.reset();
    if (scrubbing_) {
        scrubbing_ = false;
        emit scrubFinished(currentFrame_);
    }
    draggedMedia_.clear();
    dropDestination_.reset();
    unsetCursor();
    update();
    return active;
}

void TimelineWidget::setSnappingEnabled(bool enabled) {
    snappingEnabled_ = enabled;
    if (clipMove_ && clipMove_->dragging)
        updateClipMove(clipMove_->lastPosition, clipMove_->modifiers);
}

void TimelineWidget::setInsertMode(bool enabled) {
    if (insertMode_ == enabled) return;
    insertMode_ = enabled;
    update();
}

void TimelineWidget::mutate(const std::function<heisenberg::EditResult(heisenberg::ui::EditorSession&)>& command) {
    cancelInteraction();
    if (editInProgress_ || !session_) return;
    const QPointer<heisenberg::ui::EditorSession> session = session_;
    QScopedValueRollback<bool> editing(editInProgress_, true);
    emit editRequested();
    if (!session || session_ != session.data()) return;
    const auto result = command(*session);
    if (!result.changed() && !result.message.empty()) emit editFailed(QString::fromStdString(result.message));
}

void TimelineWidget::selectTrack(const std::string& trackId) {
    const auto* track = timeline() ? timeline()->trackById(trackId) : nullptr;
    if (!track) return;
    selectedTrackId_ = trackId;
    if (track->type == heisenberg::ClipType::Video) videoTargetTrackId_ = trackId;
    if (track->type == heisenberg::ClipType::Audio) audioTargetTrackId_ = trackId;
    update();
}

QString TimelineWidget::targetTrackForMedia(const QString& mediaRef) const {
    if (!session_ || !timeline()) return {};
    const auto* entry = session_->editor().manifest().find(mediaRef.toStdString());
    if (!entry || (entry->type != heisenberg::ClipType::Video && entry->type != heisenberg::ClipType::Audio)) return {};
    const auto& remembered = entry->type == heisenberg::ClipType::Video ? videoTargetTrackId_ : audioTargetTrackId_;
    const auto* track = timeline()->trackById(remembered);
    if (track && track->type == entry->type) return QString::fromStdString(track->id);
    for (const auto& candidate : timeline()->tracks)
        if (candidate.type == entry->type) return QString::fromStdString(candidate.id);
    return {};
}

void TimelineWidget::addVideoTrack() { addTrack(heisenberg::ClipType::Video); }
void TimelineWidget::addAudioTrack() { addTrack(heisenberg::ClipType::Audio); }

void TimelineWidget::addTrack(heisenberg::ClipType type) {
    mutate([this, type](heisenberg::ui::EditorSession& session) {
        auto result = session.addTrack(type);
        if (session_ == &session && result.changed() && !result.affectedIds.empty()) selectTrack(result.affectedIds.back());
        return result;
    });
}

void TimelineWidget::toggleTrack(const std::string& trackId, bool hidden) {
    const auto* current = timeline() ? timeline()->trackById(trackId) : nullptr;
    if (!current || (hidden && current->type != heisenberg::ClipType::Video)) return;
    mutate([trackId, hidden](heisenberg::ui::EditorSession& session) {
        // editRequested can replace the entire timeline when it commits a grade.
        const auto* track = session.editor().activeTimeline() ? session.editor().activeTimeline()->trackById(trackId) : nullptr;
        if (!track || (hidden && track->type != heisenberg::ClipType::Video)) return heisenberg::EditResult{};
        const bool value = hidden ? !track->hidden : !track->muted;
        return hidden ? session.setTrackHidden(QString::fromStdString(trackId), value)
                      : session.setTrackMuted(QString::fromStdString(trackId), value);
    });
}

void TimelineWidget::editSelectedClipAppearance() {
    cancelInteraction();
    const auto* clip = timeline() ? timeline()->clipById(selectedClipId_) : nullptr;
    if (!session_ || !clip || clip->mediaType != heisenberg::ClipType::Video) return;
    const QString id = QString::fromStdString(clip->id);
    const QPointer<heisenberg::ui::EditorSession> session = session_;
    const double original = clip->opacity;
    QDialog dialog(this);
    dialog.setObjectName("clipAppearanceDialog");
    dialog.setWindowTitle(tr("Clip Appearance"));
    auto* form = new QFormLayout(&dialog);
    auto* opacity = new QDoubleSpinBox(&dialog);
    opacity->setObjectName("clipOpacity");
    opacity->setRange(0.0, 100.0);
    opacity->setDecimals(2);
    opacity->setSuffix(tr(" %"));
    opacity->setValue(original * 100.0);
    form->addRow(tr("Opacity"), opacity);
    auto* hint = new QLabel(tr("Sets a fixed opacity for this clip."), &dialog);
    form->addRow(hint);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted || !session || session_ != session.data()) return;
    const double value = opacity->value() / 100.0;
    if (std::abs(value - original) < 0.000001) return;
    mutate([id, value](heisenberg::ui::EditorSession& active) { return active.setClipOpacity(id, value); });
}

void TimelineWidget::splitSelectedClip() {
    cancelInteraction();
    if (editInProgress_ || selectedClipIds_.size() > 1) return;
    if (!session_ || selectedClipId_.empty()) {
        emit editFailed(tr("Select a clip and place the playhead inside it before splitting."));
        return;
    }
    const QString id = selectedClipId();
    const qint64 frame = currentFrame_;
    const QPointer<heisenberg::ui::EditorSession> session = session_;
    QScopedValueRollback<bool> editing(editInProgress_, true);
    emit editRequested();
    if (!session || session_ != session.data()) return;
    const auto result = session->splitClip(id, frame);
    if (!result.changed() && !result.message.empty()) emit editFailed(QString::fromUtf8(result.message.c_str()));
}

void TimelineWidget::removeSelectedClip() {
    cancelInteraction();
    if (editInProgress_ || !session_ || selectedClipId_.empty()) return;
    const QStringList ids = selectedClipIds();
    const QPointer<heisenberg::ui::EditorSession> session = session_;
    QScopedValueRollback<bool> editing(editInProgress_, true);
    emit editRequested();
    if (!session || session_ != session.data()) return;
    const auto result = session->removeClips(ids);
    if (!result.changed() && !result.message.empty()) emit editFailed(QString::fromUtf8(result.message.c_str()));
}

void TimelineWidget::rippleDeleteSelectedClip() {
    cancelInteraction();
    if (editInProgress_ || !session_ || selectedClipIds_.size() != 1) return;
    const QString id = selectedClipId();
    const QPointer<heisenberg::ui::EditorSession> session = session_;
    QScopedValueRollback<bool> editing(editInProgress_, true);
    emit editRequested();
    if (!session || session_ != session.data()) return;
    const auto result = session->rippleDeleteClip(id);
    if (!result.changed() && !result.message.empty()) emit editFailed(QString::fromUtf8(result.message.c_str()));
}

void TimelineWidget::editSelectedClipAudio() {
    cancelInteraction();
    const auto* clip = timeline() ? timeline()->clipById(selectedClipId_) : nullptr;
    if (!session_ || !clip) return;
    const QString id = QString::fromStdString(clip->id);
    const QPointer<heisenberg::ui::EditorSession> session = session_;
    const int fps = framesPerSecond();
    const qint64 duration = clip->durationFrames;
    QDialog dialog(this);
    dialog.setObjectName("clipAudioDialog");
    dialog.setWindowTitle(tr("Audio Settings"));
    auto* form = new QFormLayout(&dialog);
    auto* gain = new QDoubleSpinBox(&dialog);
    gain->setObjectName("clipAudioGain");
    gain->setRange(0.0, 200.0);
    gain->setDecimals(2);
    gain->setSuffix(tr(" %"));
    gain->setValue(clip->volume * 100.0);
    auto makeFade = [&](const char* name, qint64 frames) {
        auto* spin = new QDoubleSpinBox(&dialog);
        spin->setObjectName(name);
        spin->setDecimals(6);
        spin->setRange(0.0, static_cast<double>(duration) / fps);
        spin->setSingleStep(1.0 / fps);
        spin->setSuffix(tr(" s"));
        spin->setValue(static_cast<double>(frames) / fps);
        return spin;
    };
    auto* fadeIn = makeFade("clipAudioFadeIn", clip->fadeInFrames);
    auto* fadeOut = makeFade("clipAudioFadeOut", clip->fadeOutFrames);
    form->addRow(tr("Gain"), gain);
    form->addRow(tr("Linear fade in"), fadeIn);
    form->addRow(tr("Linear fade out"), fadeOut);
    auto* hint = new QLabel(tr("Fades are rounded to timeline frames and must fit the clip."), &dialog);
    hint->setWordWrap(true);
    form->addRow(hint);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    auto validFades = [&]() {
        return std::llround(fadeIn->value() * fps) + std::llround(fadeOut->value() * fps) <= duration;
    };
    auto validate = [&]() { buttons->button(QDialogButtonBox::Ok)->setEnabled(validFades()); };
    connect(fadeIn, &QDoubleSpinBox::valueChanged, &dialog, validate);
    connect(fadeOut, &QDoubleSpinBox::valueChanged, &dialog, validate);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&]() { if (validFades()) dialog.accept(); });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    validate();
    if (dialog.exec() != QDialog::Accepted || !session || session_ != session.data()) return;
    const double volume = gain->value() / 100.0;
    const qint64 inFrames = std::llround(fadeIn->value() * fps), outFrames = std::llround(fadeOut->value() * fps);
    mutate([id, volume, inFrames, outFrames](heisenberg::ui::EditorSession& active) {
        return active.setClipAudio(id, volume, inFrames, outFrames);
    });
}

void TimelineWidget::editSelectedClipGrade() {
    cancelInteraction();
    const auto* clip = timeline() ? timeline()->clipById(selectedClipId_) : nullptr;
    if (!session_ || !clip || clip->mediaType != heisenberg::ClipType::Video) return;
    const QPointer<heisenberg::ui::EditorSession> session = session_;
    const QString id = QString::fromStdString(clip->id);
    auto effects = clip->effects.value_or(heisenberg::JsonValue::array());
    if (!effects.is_array()) { emit editFailed(tr("Unsupported effects payload is preserved.")); return; }
    size_t index = effects.size();
    for (size_t i = 0; i < effects.size(); ++i) {
        if (effects[i].is_object() && effects[i].value("type", std::string{}) == "video.basicGrade") { index = i; break; }
    }
    QDialog dialog(this);
    dialog.setObjectName("clipGradeDialog");
    dialog.setWindowTitle(tr("Basic Grade"));
    auto* form = new QFormLayout(&dialog);
    const std::array<const char*, 3> names{"exposure", "contrast", "saturation"};
    const std::array<double, 3> defaults{0.0, 1.0, 1.0};
    std::array<QDoubleSpinBox*, 3> fields{};
    std::array<double, 3> original{};
    for (size_t i = 0; i < names.size(); ++i) {
        double value = defaults[i];
        if (index < effects.size() && effects[index].contains("params") && effects[index]["params"].is_object()) {
            const auto& params = effects[index]["params"];
            if (params.contains(names[i]) && params[names[i]].is_object() && params[names[i]].contains("value") && params[names[i]]["value"].is_number())
                value = params[names[i]]["value"].get<double>();
        }
        fields[i] = new QDoubleSpinBox(&dialog);
        fields[i]->setObjectName(QString("grade_%1").arg(names[i]));
        fields[i]->setDecimals(4);
        fields[i]->setRange(i == 0 ? -10.0 : 0.0, i == 0 ? 10.0 : (i == 1 ? 4.0 : 2.0));
        fields[i]->setSingleStep(0.1);
        fields[i]->setValue(value);
        original[i] = fields[i]->value();
        form->addRow(QString::fromLatin1(names[i]), fields[i]);
    }
    auto* hint = new QLabel(tr("Changing a parameter sets a constant value and replaces its animation. Other parameters and imported effects are preserved."), &dialog);
    hint->setWordWrap(true); form->addRow(hint);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted || !session || session_ != session.data()) return;
    bool changed = false;
    for (size_t i = 0; i < fields.size(); ++i) changed |= fields[i]->value() != original[i];
    if (!changed) return;
    std::array<double, 3> values{};
    for (size_t i = 0; i < fields.size(); ++i) values[i] = fields[i]->value();
    mutate([id, names, original, values](heisenberg::ui::EditorSession& active) {
        const auto* current = active.editor().activeTimeline() ? active.editor().activeTimeline()->clipById(id.toStdString()) : nullptr;
        if (!current) return heisenberg::EditResult{};
        auto updated = current->effects.value_or(heisenberg::JsonValue::array());
        if (!updated.is_array()) return heisenberg::EditResult{heisenberg::EditStatus::Unsupported, {}, "Unsupported effects payload is preserved."};
        size_t gradeIndex = updated.size();
        for (size_t i = 0; i < updated.size(); ++i)
            if (updated[i].is_object() && updated[i].value("type", std::string{}) == "video.basicGrade") { gradeIndex = i; break; }
        if (gradeIndex == updated.size()) updated.push_back({{"id", "basic-grade"}, {"type", "video.basicGrade"}, {"enabled", true}, {"params", heisenberg::JsonValue::object()}});
        auto& params = updated[gradeIndex]["params"];
        if (!params.is_object()) params = heisenberg::JsonValue::object();
        for (size_t i = 0; i < values.size(); ++i) {
            if (values[i] == original[i]) continue;
            auto& parameter = params[names[i]];
            if (!parameter.is_object()) parameter = heisenberg::JsonValue::object();
            parameter["value"] = values[i];
            parameter.erase("track");
        }
        return active.setClipEffects(id, std::move(updated));
    });
}

QStringList TimelineWidget::selectedClipIds() const {
    QStringList result;
    if (const auto* active = timeline()) for (const auto& track : active->tracks)
        for (const auto& clip : track.clips)
            if (selectedClipIds_.contains(clip.id)) result.push_back(QString::fromStdString(clip.id));
    return result;
}

void TimelineWidget::setSelection(std::unordered_set<std::string> ids, const std::string& primary) {
    std::string id = primary;
    const auto* active = timeline();
    std::erase_if(ids, [active](const auto& value) { return !active || !active->clipById(value); });
    if (!ids.contains(id)) {
        id.clear();
        if (active) for (const auto& track : active->tracks) {
            for (const auto& clip : track.clips) if (ids.contains(clip.id)) { id = clip.id; break; }
            if (!id.empty()) break;
        }
    }
    const bool changed = selectedClipIds_ != ids;
    const bool primaryChanged = selectedClipId_ != id;
    selectedClipIds_ = std::move(ids);
    selectedClipId_ = id;
    if (active) if (const auto* track = active->trackForClip(id)) selectTrack(track->id);
    // Selection listeners can synchronously commit grading and refresh the model.
    if (primaryChanged) emit selectedClipChanged(QString::fromStdString(id));
    if (changed) emit selectedClipsChanged(selectedClipIds());
    update();
}

void TimelineWidget::setSelectedClipId(const std::string& clipId) {
    const std::string id = clipId;
    setSelection(id.empty() ? std::unordered_set<std::string>{} : std::unordered_set<std::string>{id}, id);
}

void TimelineWidget::setColorGradePanelEnabled(bool enabled) {
    colorGradePanelEnabled_ = enabled;
}

void TimelineWidget::editSelectedClipColorGrade() {
    cancelInteraction();
    const auto* clip = timeline() ? timeline()->clipById(selectedClipId_) : nullptr;
    if (!session_ || !clip || clip->mediaType != heisenberg::ClipType::Video) return;
    if (colorGradePanelEnabled_) {
        emit colorGradeRequested(selectedClipId());
        return;
    }
    const QString id = selectedClipId();
    const QPointer<heisenberg::ui::EditorSession> session = session_;
    {
        if (editInProgress_) return;
        QScopedValueRollback<bool> editing(editInProgress_, true);
        emit editRequested();
    }
    if (!session || session_ != session.data()) return;
    clip = timeline() ? timeline()->clipById(id.toStdString()) : nullptr;
    if (!clip || clip->mediaType != heisenberg::ClipType::Video) return;
    const auto original = clip->effects;
    auto effects = original.value_or(heisenberg::JsonValue::array());
    if (!effects.is_array()) { emit editFailed(tr("Unsupported effects payload is preserved.")); return; }
    for (const auto& effect : effects) {
        if (!effect.is_object() || !effect.contains("type") || effect["type"] != "video.colorGrade") continue;
        if (!ColorGradeDialog::supportsGradeSchema(effect)) {
            emit editFailed(tr("This color grade has an unsupported or invalid schemaVersion. Only integer schemaVersion 1 can be edited. The original effect is preserved unchanged."));
            return;
        }
        break; // The dialog edits the first color-grade effect only.
    }
    const auto begin = session->beginClipEffectsTransaction(id);
    if (!begin.message.empty()) { emit editFailed(QString::fromStdString(begin.message)); return; }
    ColorGradeDialog dialog(effects, this);
    dialog.setLutImporter([this, session](const QString& path, QString& lutId, QString& name, QString& error) {
        return session && session_ == session.data() && session->importGradeLut(path, lutId, name, error);
    });
    connect(&dialog, &ColorGradeDialog::effectsChanged, &dialog, [this, session, &dialog, &effects, &original] {
        if (!session || session_ != session.data()) return;
        const auto result = session->updateClipEffectsTransaction(dialog.effects() == effects ? original : std::optional<heisenberg::JsonValue>(dialog.effects()));
        if (!result.message.empty()) emit editFailed(QString::fromStdString(result.message));
    });
    const bool accepted = dialog.exec() == QDialog::Accepted;
    if (!session || session_ != session.data()) return;
    const auto result = accepted ? session->commitClipEffectsTransaction() : session->cancelClipEffectsTransaction();
    if (!result.message.empty()) emit editFailed(QString::fromStdString(result.message));
}

void TimelineWidget::trackContextMenu(const QPoint& position, const QPoint& globalPosition) {
    const auto lane = laneAt(position);
    if (!lane || !lane->track || !session_) return;
    const std::string id = lane->track->id;
    const QString name = lane->track->name ? QString::fromStdString(*lane->track->name) : QString::fromLatin1(heisenberg::toString(lane->type));
    const auto type = lane->type;
    const QPointer<heisenberg::ui::EditorSession> session = session_;
    cancelInteraction();
    selectTrack(id);
    setSelectedClipId({});
    setFocus(Qt::MouseFocusReason);
    auto neighbor = [this, id, type](int direction) -> int {
        if (!timeline()) return -1;
        const auto& tracks = timeline()->tracks;
        int index = -1;
        for (int i = 0; i < static_cast<int>(tracks.size()); ++i) if (tracks[i].id == id) { index = i; break; }
        if (index < 0) return -1;
        for (int i = index + direction; i >= 0 && i < static_cast<int>(tracks.size()); i += direction)
            if (tracks[i].type == type) return i;
        return -1;
    };
    QMenu menu(this);
    menu.setObjectName("timelineTrackMenu");
    auto* rename = menu.addAction(tr("Rename Track..."));
    rename->setObjectName("trackRename");
    auto* up = menu.addAction(tr("Move Track Up"));
    up->setObjectName("trackMoveUp");
    up->setEnabled(neighbor(-1) >= 0);
    auto* down = menu.addAction(tr("Move Track Down"));
    down->setObjectName("trackMoveDown");
    down->setEnabled(neighbor(1) >= 0);
    menu.addSeparator();
    auto* remove = menu.addAction(tr("Delete Track..."));
    remove->setObjectName("trackDelete");
    auto* chosen = menu.exec(globalPosition);
    if (!session || session_ != session.data()) return;
    const QString trackId = QString::fromStdString(id);
    if (chosen == rename) {
        bool ok = false;
        const QString value = QInputDialog::getText(this, tr("Rename Track"), tr("Track name"), QLineEdit::Normal, name, &ok).trimmed();
        if (ok && !value.isEmpty() && session && session_ == session.data())
            mutate([trackId, value](heisenberg::ui::EditorSession& active) { return active.renameTrack(trackId, value); });
    } else if (chosen == up || chosen == down) {
        const int direction = chosen == up ? -1 : 1;
        mutate([trackId, direction, neighbor](heisenberg::ui::EditorSession& active) {
            const int index = neighbor(direction);
            return index < 0 ? heisenberg::EditResult{} : active.reorderTrack(trackId, index);
        });
    } else if (chosen == remove) {
        const bool populated = [this, &id] {
            const auto* track = timeline() ? timeline()->trackById(id) : nullptr;
            return track && !track->clips.empty();
        }();
        if (populated && QMessageBox::question(this, tr("Delete Track"),
            tr("Delete \"%1\" and all of its clips? This can be undone.").arg(name),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
        if (session && session_ == session.data())
            mutate([trackId](heisenberg::ui::EditorSession& active) { return active.removeTrack(trackId); });
    }
}

void TimelineWidget::contextMenuEvent(QContextMenuEvent* event) {
    if (event->pos().x() < kHeaderWidth && laneAt(event->pos())) {
        trackContextMenu(event->pos(), event->globalPos());
        event->accept();
        return;
    }
    const auto hit = clipAt(event->pos());
    if (hit.empty()) { QWidget::contextMenuEvent(event); return; }
    cancelInteraction();
    if (!selectedClipIds_.contains(hit)) setSelectedClipId(hit);
    else setSelection(selectedClipIds_, hit);
    setFocus(Qt::MouseFocusReason);
    update();
    QMenu menu(this);
    menu.setObjectName("timelineClipMenu");
    auto* audio = menu.addAction(tr("Audio Settings..."));
    audio->setObjectName("clipAudioSettings");
    auto* colorGrade = menu.addAction(tr("Color Grade..."));
    colorGrade->setObjectName("clipColorGrade");
    auto* grade = menu.addAction(tr("Basic Grade..."));
    grade->setObjectName("clipBasicGrade");
    const bool videoSelected = [this] {
        const auto* selected = timeline() ? timeline()->clipById(selectedClipId_) : nullptr;
        return selected && selected->mediaType == heisenberg::ClipType::Video;
    }();
    grade->setEnabled(videoSelected);
    colorGrade->setEnabled(videoSelected);
    auto* appearance = menu.addAction(tr("Clip Appearance..."));
    appearance->setObjectName("clipAppearance");
    appearance->setEnabled(videoSelected);
    auto* remove = menu.addAction(selectedClipIds_.size() > 1 ? tr("Delete Selected Clips") : tr("Delete Clip"));
    auto* rippleDelete = menu.addAction(tr("Ripple Delete"));
    rippleDelete->setEnabled(selectedClipIds_.size() == 1);
    rippleDelete->setObjectName("clipRippleDelete");
    const auto* chosen = menu.exec(event->globalPos());
    if (chosen == audio) editSelectedClipAudio();
    else if (chosen == colorGrade) editSelectedClipColorGrade();
    else if (chosen == grade) editSelectedClipGrade();
    else if (chosen == appearance) editSelectedClipAppearance();
    else if (chosen == remove) removeSelectedClip();
    else if (chosen == rippleDelete) rippleDeleteSelectedClip();
    event->accept();
}

void TimelineWidget::setPlaying(bool playing) {
    if (playing_ == playing) return;
    playing_ = playing;
    update();
}

void TimelineWidget::fitToContent() {
    const qint64 frames = contentFrameCount();
    const double available = std::max(1.0, contentRight() - kHeaderWidth - 24.0);
    pixelsPerFrame_ = frames > 0
        ? std::clamp(available / static_cast<double>(frames), kMinZoom, kMaxZoom)
        : 4.0;
    scrollOffset_ = 0.0;
    update();
}

void TimelineWidget::zoomIn() {
    setZoom(pixelsPerFrame_ * 1.25, width() * 0.6);
}
void TimelineWidget::zoomOut() {
    setZoom(pixelsPerFrame_ / 1.25, width() * 0.6);
}

QString TimelineWidget::timelineName() const {
    const auto* active = timeline();
    return active ? QString::fromUtf8(active->name.c_str()) : tr("Timeline");
}

int TimelineWidget::framesPerSecond() const {
    const auto* active = timeline();
    return active && active->fps > 0 ? active->fps : 30;
}

QSize TimelineWidget::sizeHint() const {
    return QSize(960, std::max(210, minimumSizeHint().height()));
}
QSize TimelineWidget::minimumSizeHint() const {
    return QSize(480, 142);
}

int TimelineWidget::contentRight() const {
    return width() - (verticalScrollBar_->isHidden() ? 0 : verticalScrollBar_->width());
}

void TimelineWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    updateVerticalScrollBar();
    clampView();
    if (clipMove_ && clipMove_->dragging) updateClipMove(clipMove_->lastPosition, clipMove_->modifiers);
    if (marquee_ && marquee_->dragging) updateMarquee(marquee_->lastPosition);
}

void TimelineWidget::updateVerticalScrollBar() {
    double total = kNewVideoTrackStripHeight;
    for (const auto& lane : lanes()) total += lane.height;
    const int viewport = std::max(0, height() - static_cast<int>(kRulerHeight));
    const int maximum = std::max(0, static_cast<int>(std::ceil(total)) - viewport);
    const QSignalBlocker blocker(verticalScrollBar_);
    verticalScrollBar_->setPageStep(viewport);
    verticalScrollBar_->setRange(0, maximum);
    verticalScrollBar_->setVisible(maximum > 0);
    const int extent = style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, this);
    verticalScrollBar_->setGeometry(std::max(0, width() - extent), static_cast<int>(kRulerHeight), extent, viewport);
    verticalScrollBar_->raise();
}

const heisenberg::Timeline* TimelineWidget::timeline() const {
    return session_ ? session_->editor().activeTimeline() : nullptr;
}

qint64 TimelineWidget::contentFrameCount() const {
    const qint64 projectFrames = timeline() ? timeline()->totalFrames() : 0;
    return std::max<qint64>(1, std::max(projectFrames, playbackFrameCount_));
}

qint64 TimelineWidget::frameAtX(double x) const {
    const double frame = (x - kHeaderWidth + scrollOffset_) / pixelsPerFrame_;
    return std::clamp<qint64>(static_cast<qint64>(std::floor(frame)), 0,
                              std::max<qint64>(0, contentFrameCount() - 1));
}

double TimelineWidget::xForFrame(qint64 frame) const {
    return kHeaderWidth + static_cast<double>(frame) * pixelsPerFrame_ - scrollOffset_;
}

double TimelineWidget::maximumScrollOffset() const {
    const double viewport = std::max(1.0, contentRight() - kHeaderWidth);
    return std::max(0.0, static_cast<double>(contentFrameCount()) * pixelsPerFrame_ - viewport + 24.0);
}

void TimelineWidget::clampView() {
    pixelsPerFrame_ = std::clamp(pixelsPerFrame_, kMinZoom, kMaxZoom);
    scrollOffset_ = std::clamp(scrollOffset_, 0.0, maximumScrollOffset());
}

void TimelineWidget::setZoom(double zoom, double anchorX) {
    const double boundedAnchor = std::clamp(anchorX, kHeaderWidth, std::max(kHeaderWidth, static_cast<double>(contentRight())));
    const double anchorFrame = (boundedAnchor - kHeaderWidth + scrollOffset_) / pixelsPerFrame_;
    pixelsPerFrame_ = std::clamp(zoom, kMinZoom, kMaxZoom);
    scrollOffset_ = anchorFrame * pixelsPerFrame_ - (boundedAnchor - kHeaderWidth);
    clampView();
    if (clipMove_ && clipMove_->dragging) updateClipMove(clipMove_->lastPosition, clipMove_->modifiers);
    if (marquee_ && marquee_->dragging) updateMarquee(marquee_->lastPosition);
    update();
}

QString TimelineWidget::formatTimecode(qint64 frame) const {
    const qint64 fps = std::max(1, framesPerSecond());
    const qint64 totalSeconds = frame / fps;
    const qint64 hours = totalSeconds / 3600;
    const qint64 minutes = (totalSeconds / 60) % 60;
    const qint64 seconds = totalSeconds % 60;
    const qint64 frames = frame % fps;
    return QString("%1:%2:%3:%4")
        .arg(hours, 2, 10, QChar('0')).arg(minutes, 2, 10, QChar('0'))
        .arg(seconds, 2, 10, QChar('0')).arg(frames, 2, 10, QChar('0'));
}

QColor TimelineWidget::clipColor(heisenberg::ClipType type, bool missing) const {
    if (missing) return QColor(126, 47, 49);
    switch (type) {
    case heisenberg::ClipType::Video: return QColor(0x1d, 0x58, 0x78);
    case heisenberg::ClipType::Audio: return QColor(0x2e, 0x77, 0x65);
    case heisenberg::ClipType::Image: return QColor(0x71, 0x54, 0x86);
    case heisenberg::ClipType::Text:
    case heisenberg::ClipType::Subtitle: return QColor(0x71, 0x54, 0x86);
    case heisenberg::ClipType::Lottie: return QColor(0xa0, 0x78, 0x22);
    case heisenberg::ClipType::Sequence: return QColor(0xb9, 0xb2, 0x9a);
    }
    return QColor(0x1d, 0x58, 0x78);
}

QString TimelineWidget::clipName(const heisenberg::Clip& clip) const {
    const auto found = mediaNames_.find(clip.mediaRef);
    if (found != mediaNames_.end()) return QString::fromUtf8(found->second.c_str());
    if (clip.textContent && !clip.textContent->empty()) return QString::fromUtf8(clip.textContent->c_str());
    return QString::fromUtf8(heisenberg::toString(clip.mediaType));
}

void TimelineWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), kBase);

    const QRectF ruler(kHeaderWidth, 0, std::max(0.0, width() - kHeaderWidth), kRulerHeight);
    painter.fillRect(ruler, kSurface);
    painter.fillRect(QRectF(0, 0, kHeaderWidth, kRulerHeight), kRaised);
    painter.setPen(kBorder);
    painter.drawLine(QPointF(0, kRulerHeight - 0.5), QPointF(width(), kRulerHeight - 0.5));
    painter.drawLine(QPointF(kHeaderWidth - 0.5, 0), QPointF(kHeaderWidth - 0.5, height()));

    QFont smallFont = font();
    smallFont.setPixelSize(10);
    painter.setFont(smallFont);
    painter.setPen(QColor(245, 153, 51));
    painter.drawText(QRectF(10, 0, kHeaderWidth - 20, kRulerHeight),
                     Qt::AlignVCenter | Qt::AlignLeft, formatTimecode(currentFrame_));

    const int fps = framesPerSecond();
    const double targetPixels = 80.0;
    const double rawFrames = targetPixels / pixelsPerFrame_;
    const std::array<int, 13> seconds = {1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1200, 1800, 3600};
    qint64 majorFrames = static_cast<qint64>(seconds.back()) * fps;
    for (const int candidate : seconds) {
        const qint64 frames = static_cast<qint64>(candidate) * fps;
        if (static_cast<double>(frames) >= rawFrames) { majorFrames = frames; break; }
    }
    const double majorPixels = static_cast<double>(majorFrames) * pixelsPerFrame_;
    int subdivisions = 0;
    for (const int candidate : {20, 10, 5, 4, 2}) {
        if (majorPixels / candidate >= 6.0 && majorFrames % candidate == 0) {
            subdivisions = candidate;
            break;
        }
    }
    const qint64 minorFrames = subdivisions > 0 ? majorFrames / subdivisions : 0;
    const qint64 firstFrame = std::max<qint64>(0, static_cast<qint64>(scrollOffset_ / pixelsPerFrame_) - majorFrames);
    const qint64 lastFrame = static_cast<qint64>((scrollOffset_ + ruler.width()) / pixelsPerFrame_) + majorFrames;

    if (minorFrames > 0) {
        painter.setPen(QPen(QColor(255, 255, 255, 40), 0.5));
        qint64 frame = (firstFrame / minorFrames) * minorFrames;
        for (; frame <= lastFrame; frame += minorFrames) {
            if (frame % majorFrames == 0) continue;
            const double x = xForFrame(frame);
            if (x < kHeaderWidth || x > width()) continue;
            const bool midpoint = subdivisions % 2 == 0 && frame % (majorFrames / 2) == 0;
            painter.drawLine(QPointF(x, kRulerHeight), QPointF(x, kRulerHeight - (midpoint ? 8.0 : 4.0)));
        }
    }

    painter.setPen(QPen(kMutedText, 1));
    qint64 major = (firstFrame / majorFrames) * majorFrames;
    for (; major <= lastFrame; major += majorFrames) {
        const double x = xForFrame(major);
        if (x < kHeaderWidth || x > width()) continue;
        painter.drawLine(QPointF(x, kRulerHeight), QPointF(x, kRulerHeight - 10));
        painter.setPen(kTertiaryText);
        painter.drawText(QRectF(x + 4, 2, 100, 16), Qt::AlignLeft | Qt::AlignVCenter,
                         formatTimecode(major).left(8));
        painter.setPen(QPen(kMutedText, 1));
    }

    painter.save();
    painter.setClipRect(QRectF(0, kRulerHeight, contentRight(), std::max(0.0, height() - kRulerHeight)));
    const auto* active = timeline();
    const double stripTop = kRulerHeight - verticalScrollBar_->value();
    const QRectF strip(kHeaderWidth, stripTop, std::max(0.0, contentRight() - kHeaderWidth),
                       kNewVideoTrackStripHeight);
    painter.fillRect(QRectF(0, stripTop, kHeaderWidth, kNewVideoTrackStripHeight), kSurface);
    painter.fillRect(strip, kRaised);
    painter.setFont(smallFont);
    painter.setPen(kMutedText);
    painter.drawText(strip.adjusted(8, 0, -8, 0), Qt::AlignLeft | Qt::AlignVCenter,
                     tr("Drop video to create a new track"));
    painter.setPen(kSubtleBorder);
    painter.drawLine(strip.bottomLeft(), strip.bottomRight());
    double y = strip.bottom();
    for (const auto& lane : lanes()) {
        const double trackHeight = lane.height;
        y = lane.top;
        if (y + trackHeight <= kRulerHeight || y >= height()) continue;
        if (!lane.track) {
            const QRectF headerRect(0, y, kHeaderWidth, trackHeight);
            const QRectF laneRect(kHeaderWidth, y, width() - kHeaderWidth, trackHeight);
            painter.fillRect(headerRect, kSurface);
            painter.fillRect(laneRect, QColor(20, 21, 24));
            painter.setPen(kMutedText);
            painter.drawText(headerRect.adjusted(12, 0, -8, 0), Qt::AlignVCenter | Qt::AlignLeft,
                             lane.type == heisenberg::ClipType::Audio ? tr("AUDIO 1") : tr("VIDEO 1"));
            painter.drawText(laneRect.adjusted(14, 0, -8, 0), Qt::AlignVCenter | Qt::AlignLeft,
                             lane.type == heisenberg::ClipType::Audio ? tr("Drop audio here") : tr("Drop video here"));
            y += trackHeight;
            continue;
        }
        const auto& track = *lane.track;
            const QRectF headerRect(0, y, kHeaderWidth, trackHeight);
            const QRectF laneRect(kHeaderWidth, y, width() - kHeaderWidth, trackHeight);
            painter.fillRect(headerRect, selectedTrackId_ == track.id ? kProminent : kSurface);
            painter.fillRect(laneRect, QColor(20, 21, 24));
            painter.fillRect(QRectF(0, y, 3, trackHeight), clipColor(track.type, false));
            painter.setPen(kSubtleBorder);
            painter.drawLine(QPointF(0, y + trackHeight - 0.5), QPointF(width(), y + trackHeight - 0.5));

            QFont trackFont = font();
            trackFont.setPixelSize(11);
            trackFont.setBold(true);
            painter.setFont(trackFont);
            painter.setPen(kPrimaryText);
            const QString trackName = track.name
                ? QString::fromUtf8(track.name->c_str())
                : QString::fromUtf8(heisenberg::toString(track.type)).toUpper();
            const int nameWidth = track.type == heisenberg::ClipType::Video ? 84 : 112;
            painter.drawText(QRectF(12, y + 5, nameWidth, 17), Qt::AlignLeft | Qt::AlignVCenter,
                             painter.fontMetrics().elidedText(trackName, Qt::ElideRight, nameWidth));
            if (track.type == heisenberg::ClipType::Video) {
                const QRectF hiddenRect(kHeaderWidth - 60, y + 8, 24, 22);
                painter.setBrush(track.hidden ? QColor(84, 91, 145) : kRaised);
                painter.setPen(kBorder);
                painter.drawRoundedRect(hiddenRect, 3, 3);
                painter.setPen(kPrimaryText);
                painter.drawText(hiddenRect, Qt::AlignCenter, tr("H"));
            }
            const QRectF muteRect(kHeaderWidth - 32, y + 8, 24, 22);
            painter.setBrush(track.muted ? QColor(169, 103, 44) : kRaised);
            painter.setPen(kBorder);
            painter.drawRoundedRect(muteRect, 3, 3);
            painter.setPen(kPrimaryText);
            painter.drawText(muteRect, Qt::AlignCenter, tr("M"));

            QFont statusFont = font();
            statusFont.setPixelSize(9);
            painter.setFont(statusFont);
            painter.setPen(kMutedText);
            QStringList states;
            if (track.muted) states << "M";
            if (track.hidden) states << "H";
            if (track.syncLocked) states << "L";
            painter.drawText(QRectF(12, y + 23, 130, 14), Qt::AlignLeft | Qt::AlignVCenter,
                             states.isEmpty() ? tr("Ready") : states.join("  "));

            painter.save();
            painter.setClipRect(laneRect, Qt::IntersectClip);
            for (const auto& clip : track.clips) {
                const bool moving = clipMove_ && clipMove_->dragging && std::any_of(
                    clipMove_->members.begin(), clipMove_->members.end(), [&](const auto& member) { return member.clipId == clip.id; });
                painter.setOpacity(moving ? 0.35 : 1.0);
                const QRectF clipRect = clipRectangle(clip, lane);
                if (clipRect.right() < kHeaderWidth || clipRect.left() > width()) continue;
                const bool missing = missingMedia_.contains(clip.mediaRef);
                const QColor color = clipColor(clip.sourceClipType, missing);
                QPainterPath path;
                path.addRoundedRect(clipRect, 4, 4);
                painter.fillPath(path, color);
                painter.fillRect(QRectF(clipRect.left(), clipRect.top(), clipRect.width(),
                                        std::min(16.0, clipRect.height())), color.lighter(112));
                const bool selected = marquee_ && marquee_->dragging ? marquee_->hits.contains(clip.id) : selectedClipIds_.contains(clip.id);
                const bool primary = selected && selectedClipId_ == clip.id;
                const bool hovered = hoveredClipId_ == clip.id;
                painter.setPen(QPen(selected ? (primary ? QColor(255, 255, 255) : QColor(112, 199, 238)) : QColor(0, 0, 0, hovered ? 210 : 150),
                                    primary ? 2.0 : selected ? 1.5 : 1.0));
                painter.drawPath(path);

                if (selected || hovered) {
                    painter.setPen(QPen(QColor(255, 255, 255, selected ? 230 : 140), 2));
                    const double inset = std::min(3.0, clipRect.width() / 4.0);
                    painter.drawLine(QPointF(clipRect.left() + inset, clipRect.top() + 5),
                                     QPointF(clipRect.left() + inset, clipRect.bottom() - 5));
                    painter.drawLine(QPointF(clipRect.right() - inset, clipRect.top() + 5),
                                     QPointF(clipRect.right() - inset, clipRect.bottom() - 5));
                }

                if (clipRect.width() >= 34) {
                    QFont labelFont = font();
                    labelFont.setPixelSize(10);
                    labelFont.setBold(true);
                    painter.setFont(labelFont);
                    painter.setPen(QColor(255, 255, 255, 235));
                    const QRectF labelRect = clipRect.adjusted(6, 1, -5, -1);
                    painter.drawText(labelRect, Qt::AlignTop | Qt::AlignLeft,
                                     painter.fontMetrics().elidedText(clipName(clip), Qt::ElideRight,
                                                                       static_cast<int>(labelRect.width())));
                    if (clipRect.width() >= 92) {
                        QFont detailFont = labelFont;
                        detailFont.setBold(false);
                        detailFont.setPixelSize(9);
                        painter.setFont(detailFont);
                        painter.setPen(QColor(255, 255, 255, 180));
                        const QString detail = missing
                            ? tr("Offline media")
                            : tr("%1 frames%2").arg(clip.durationFrames)
                                  .arg(std::abs(clip.speed - 1.0) > 0.0001
                                           ? tr("  %1x").arg(clip.speed, 0, 'g', 3) : QString());
                        painter.drawText(clipRect.adjusted(6, 17, -5, -2),
                                         Qt::AlignLeft | Qt::AlignVCenter, detail);
                    }
                }
            }
            painter.setOpacity(1.0);
            painter.restore();
            y += trackHeight;
    }
    painter.restore();

    if (active) {
        painter.save();
        painter.setClipRect(QRectF(kHeaderWidth, 0, width() - kHeaderWidth, height()));
        for (const auto& marker : active->markers) {
            const double x = xForFrame(marker.startFrame);
            const QColor markerColor = QColor::fromRgbF(
                std::clamp(marker.color.r, 0.0, 1.0), std::clamp(marker.color.g, 0.0, 1.0),
                std::clamp(marker.color.b, 0.0, 1.0), std::clamp(marker.color.a, 0.0, 1.0));
            painter.setPen(QPen(markerColor, 2));
            painter.drawLine(QPointF(x, 17), QPointF(x, y));
            QPainterPath flag;
            flag.moveTo(x, 17);
            flag.lineTo(x + 9, 20);
            flag.lineTo(x, 25);
            flag.closeSubpath();
            painter.fillPath(flag, markerColor);
        }
        painter.restore();
    }

    if (!draggedMedia_.isEmpty() && dropDestination_ && session_ && active) {
        const auto* entry = session_->editor().manifest().find(draggedMedia_.toStdString());
        qint64 trim = 0, duration = 0;
        if (entry && session_->sourcePlacementRange(draggedMedia_, trim, duration)) {
            const double ghostWidth = std::max(2.0, duration * pixelsPerFrame_);
            painter.save();
            painter.setClipRect(QRectF(kHeaderWidth, kRulerHeight, contentRight() - kHeaderWidth, height() - kRulerHeight));
            if (dropDestination_->createNewTrack) {
                painter.fillRect(QRectF(kHeaderWidth, dropDestination_->top, contentRight() - kHeaderWidth,
                                        dropDestination_->height), QColor(112, 199, 238, 45));
            }
            painter.setPen(QPen(QColor(255, 255, 255, 220), 1, Qt::DashLine));
            QColor ghostColor = clipColor(entry->type, false);
            ghostColor.setAlpha(95);
            painter.setBrush(ghostColor);
            painter.drawRoundedRect(QRectF(xForFrame(dropFrame_), dropDestination_->top + 3,
                                           ghostWidth, dropDestination_->height - 6), 4, 4);
            painter.setPen(kPrimaryText);
            const QString label = (dropDestination_->createNewTrack ? tr("Create video track — ") : QString()) +
                (insertMode_ ? tr("Insert at %1") : tr("Overwrite at %1")).arg(formatTimecode(dropFrame_));
            if (dropDestination_->createNewTrack) {
                painter.drawText(QRectF(kHeaderWidth + 8, dropDestination_->top, contentRight() - kHeaderWidth - 16,
                                        dropDestination_->height), Qt::AlignLeft | Qt::AlignVCenter, label);
            } else {
                painter.drawText(QPointF(xForFrame(dropFrame_) + 6, dropDestination_->top + dropDestination_->height + 18), label);
            }
            painter.restore();
        }
    }

    if (clipMove_ && clipMove_->dragging && active && !active->tracks.empty()) {
        const auto& move = *clipMove_;
        const auto lane = laneForTrack(move.targetTrackId.empty() ? move.trackId : move.targetTrackId);
        const double trackHeight = move.createNewTrack ? kNewVideoTrackStripHeight : lane ? lane->height : kDefaultTrackHeight;
        const double trackTop = move.createNewTrack ? kRulerHeight - verticalScrollBar_->value() : lane ? lane->top : kRulerHeight;
        painter.save();
        painter.setClipRect(QRectF(kHeaderWidth, kRulerHeight, contentRight() - kHeaderWidth, height() - kRulerHeight));
        const QColor accent = move.validDrop ? QColor(112, 199, 238) : QColor(229, 122, 122);
        if (move.createNewTrack && move.validDrop)
            painter.fillRect(QRectF(kHeaderWidth, trackTop, contentRight() - kHeaderWidth, trackHeight), QColor(112, 199, 238, 45));
        painter.setPen(QPen(accent, 1.5, Qt::DashLine));
        painter.setBrush(QColor(accent.red(), accent.green(), accent.blue(), 95));
        const QRectF ghost(xForFrame(move.targetFrame), trackTop + 3,
                           std::max(2.0, move.durationFrames * pixelsPerFrame_), trackHeight - 6);
        if (move.trimEdge) painter.drawRoundedRect(ghost, 4, 4);
        else for (const auto& member : move.members) {
            const auto targetLane = laneForTrack(member.targetTrackId.empty() ? member.trackId : member.targetTrackId);
            const double top = move.createNewTrack ? trackTop : targetLane ? targetLane->top : trackTop;
            const double height = move.createNewTrack ? trackHeight : targetLane ? targetLane->height : trackHeight;
            painter.drawRoundedRect(QRectF(xForFrame(member.targetFrame), top + 3,
                std::max(2.0, member.durationFrames * pixelsPerFrame_), height - 6), 4, 4);
        }
        if (move.snapFrame && move.validDrop) {
            const double snapX = xForFrame(*move.snapFrame);
            painter.setPen(QPen(QColor(245, 199, 98), 1.5));
            painter.drawLine(QPointF(snapX, 0), QPointF(snapX, height()));
        }
        painter.setPen(accent);
        const double labelX = std::clamp(ghost.left() + 6, kHeaderWidth + 6, std::max(kHeaderWidth + 6, width() - 230.0));
        QString label = move.trimEdge ? tr("Release in the original lane to trim") : tr("Choose a track of the same type");
        if (move.validDrop) {
            label = move.trimEdge
                ? tr("%1  %2  /  %3 frames").arg(*move.trimEdge == heisenberg::TrimEdge::Left ? tr("Trim In") : tr("Trim Out"))
                      .arg(formatTimecode(move.targetEdgeFrame)).arg(move.durationFrames)
                : (move.createNewTrack ? tr("Create video track — Move at %1") : tr("Move / overwrite at %1"))
                      .arg(formatTimecode(move.targetFrame));
            if (move.snapFrame) label += tr("  [Snap]");
            if (move.sourceLimited) label += tr("  [Limit]");
        }
        if (move.trimEdge) {
            painter.setPen(QPen(accent, 3));
            painter.drawLine(QPointF(xForFrame(move.targetEdgeFrame), ghost.top()),
                             QPointF(xForFrame(move.targetEdgeFrame), ghost.bottom()));
        }
        painter.setPen(accent);
        if (move.createNewTrack) {
            painter.drawText(QRectF(kHeaderWidth + 8, trackTop, contentRight() - kHeaderWidth - 16, trackHeight),
                             Qt::AlignLeft | Qt::AlignVCenter, label);
        } else painter.drawText(QPointF(labelX, trackTop + trackHeight + 18), label);
        painter.restore();
    }

    if (marquee_ && marquee_->dragging) {
        painter.save();
        painter.setClipRect(QRectF(kHeaderWidth, kRulerHeight, contentRight() - kHeaderWidth, height() - kRulerHeight));
        painter.setPen(QPen(QColor(112, 199, 238), 1));
        painter.setBrush(QColor(112, 199, 238, 40));
        painter.drawRect(marquee_->rectangle);
        painter.restore();
    }

    const double playheadX = xForFrame(currentFrame_);
    if (playheadX >= kHeaderWidth && playheadX <= width()) {
        painter.save();
        painter.setPen(QPen(kPlayhead, 1.5));
        painter.drawLine(QPointF(playheadX, 0), QPointF(playheadX, height()));
        QPainterPath handle;
        handle.moveTo(playheadX - 5, 0);
        handle.lineTo(playheadX + 5, 0);
        handle.lineTo(playheadX + 5, 7);
        handle.lineTo(playheadX, 12);
        handle.lineTo(playheadX - 5, 7);
        handle.closeSubpath();
        painter.fillPath(handle, kPlayhead);
        painter.restore();
    }
}

std::vector<TimelineWidget::Lane> TimelineWidget::lanes() const {
    std::vector<Lane> result;
    double top = kRulerHeight + kNewVideoTrackStripHeight - verticalScrollBar_->value();
    const auto* active = timeline();
    for (const auto type : {heisenberg::ClipType::Video, heisenberg::ClipType::Audio}) {
        bool found = false;
        if (active) for (const auto& track : active->tracks) {
            if (track.type != type) continue;
            result.push_back({&track, type, top, boundedTrackHeight(track.displayHeight)});
            top += result.back().height;
            found = true;
        }
        if (!found) {
            result.push_back({nullptr, type, top, kDefaultTrackHeight});
            top += kDefaultTrackHeight;
        }
    }
    if (active) for (const auto& track : active->tracks) {
        if (track.type == heisenberg::ClipType::Video || track.type == heisenberg::ClipType::Audio) continue;
        result.push_back({&track, track.type, top, boundedTrackHeight(track.displayHeight)});
        top += result.back().height;
    }
    return result;
}

std::optional<TimelineWidget::Lane> TimelineWidget::laneAt(const QPointF& point) const {
    if (point.y() < kRulerHeight || point.y() >= height() || point.x() < 0 || point.x() >= contentRight()) return std::nullopt;
    for (const auto& lane : lanes())
        if (point.y() >= lane.top && point.y() < lane.top + lane.height) return lane;
    return std::nullopt;
}

std::optional<TimelineWidget::Lane> TimelineWidget::laneForTrack(const std::string& trackId) const {
    for (const auto& lane : lanes())
        if (lane.track && lane.track->id == trackId) return lane;
    return std::nullopt;
}

std::string TimelineWidget::clipAt(const QPointF& point) const {
    if (point.x() < kHeaderWidth) return {};
    const auto lane = laneAt(point);
    if (!lane || !lane->track) return {};
    // Do not clamp empty space after the last clip to its final frame.
    const qint64 frame = dropFrameAtX(point.x());
    for (const auto& clip : lane->track->clips)
        if (clip.contains(frame)) return clip.id;
    return {};
}

QRectF TimelineWidget::clipRectangle(const heisenberg::Clip& clip, const Lane& lane) const {
    return QRectF(xForFrame(clip.startFrame), lane.top + 3,
                  std::max(1.0, clip.durationFrames * pixelsPerFrame_), lane.height - 6);
}

void TimelineWidget::updateMarquee(const QPointF& point) {
    if (!marquee_) return;
    auto& draft = *marquee_;
    draft.lastPosition = point;
    if (!draft.dragging && (point - draft.pressPosition).manhattanLength() >= QApplication::startDragDistance())
        draft.dragging = true;
    if (!draft.dragging) return;
    const QPointF anchor(kHeaderWidth + draft.anchor.x() * pixelsPerFrame_ - scrollOffset_,
                         draft.anchor.y() - verticalScrollBar_->value());
    const QPointF end(std::clamp(point.x(), kHeaderWidth, std::max(kHeaderWidth, double(contentRight()))),
                      std::clamp(point.y(), kRulerHeight, std::max(kRulerHeight, double(height()))));
    draft.rectangle = QRectF(anchor, end).normalized();
    draft.hits = draft.additive ? draft.baseline : std::unordered_set<std::string>{};
    const QRectF viewport(kHeaderWidth, kRulerHeight, std::max(0.0, contentRight() - kHeaderWidth),
                          std::max(0.0, height() - kRulerHeight));
    const QRectF selection = draft.rectangle.intersected(viewport);
    for (const auto& lane : lanes()) if (lane.track)
        for (const auto& clip : lane.track->clips)
            if (selection.intersects(clipRectangle(clip, lane))) draft.hits.insert(clip.id);
    update();
}

void TimelineWidget::updateScrub(double x, bool finished) {
    currentFrame_ = frameAtX(x);
    if (finished) emit scrubFinished(currentFrame_);
    else emit scrubFrameRequested(currentFrame_);
    update();
}

std::optional<heisenberg::TrimEdge> TimelineWidget::trimEdgeAt(const heisenberg::Clip& clip, double x) const {
    const double left = xForFrame(clip.startFrame), right = xForFrame(clip.endFrame());
    // Keep a usable move region even for narrow clips. Clipped-off edges are not handles.
    const double handleWidth = std::min(kTrimHandleWidth, (right - left) / 4.0);
    if (left >= kHeaderWidth && x - left <= handleWidth) return heisenberg::TrimEdge::Left;
    if (right <= contentRight() && right - x <= handleWidth) return heisenberg::TrimEdge::Right;
    return std::nullopt;
}

void TimelineWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::RightButton) {
        const auto hit = clipAt(event->position());
        if (!hit.empty()) {
            cancelInteraction();
            if (!selectedClipIds_.contains(hit)) setSelectedClipId(hit);
            else setSelection(selectedClipIds_, hit);
            setFocus(Qt::MouseFocusReason);
            update();
        }
    }
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    if (event->position().x() < kHeaderWidth) {
        const auto lane = laneAt(event->position());
        if (session_ && lane && lane->track) {
            const std::string id = lane->track->id;
            const bool video = lane->type == heisenberg::ClipType::Video;
            const double relativeY = event->position().y() - lane->top;
            const bool mute = relativeY >= 8 && relativeY < 30 && event->position().x() >= kHeaderWidth - 32 && event->position().x() < kHeaderWidth - 8;
            const bool hidden = video && relativeY >= 8 && relativeY < 30 && event->position().x() >= kHeaderWidth - 60 && event->position().x() < kHeaderWidth - 36;
            setFocus(Qt::MouseFocusReason);
            cancelInteraction();
            selectTrack(id);
            if (mute || hidden) toggleTrack(id, hidden);
            else setSelectedClipId({});
            event->accept();
        } else QWidget::mousePressEvent(event);
        return;
    }
    setFocus(Qt::MouseFocusReason);
    emit timelineActivated();
    cancelInteraction();
    const auto hit = clipAt(event->position());
    if (!hit.empty()) {
        // Selection listeners may commit grading and synchronously replace the timeline.
        if (event->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier)) {
            auto ids = selectedClipIds_;
            if (ids.contains(hit)) ids.erase(hit); else ids.insert(hit);
            setSelection(std::move(ids), hit);
            event->accept();
            return;
        }
        if (!selectedClipIds_.contains(hit)) setSelectedClipId(hit);
        else setSelection(selectedClipIds_, hit);
        const auto* active = timeline();
        const auto* clip = active ? active->clipById(hit) : nullptr;
        const auto* track = active ? active->trackForClip(hit) : nullptr;
        if (!clip || !track) {
            event->accept();
            return;
        }
        ClipMove move;
        move.timelineId = active->id;
        for (const auto& owner : active->tracks) for (const auto& member : owner.clips)
            if (selectedClipIds_.contains(member.id))
                move.members.push_back({member.id, owner.id, owner.id, owner.type,
                                        member.startFrame, member.durationFrames, member.startFrame});
        move.clipId = hit;
        move.trackId = move.targetTrackId = track->id;
        move.type = track->type;
        move.pressPosition = move.lastPosition = event->position();
        if (move.members.size() == 1) move.trimEdge = trimEdgeAt(*clip, event->position().x());
        move.originalStartFrame = clip->startFrame;
        move.originalEndFrame = clip->endFrame();
        const bool leftTrim = move.trimEdge == heisenberg::TrimEdge::Left;
        const qint64 grabFrame = move.trimEdge && !leftTrim ? move.originalEndFrame : move.originalStartFrame;
        move.grabOffsetFrames = (event->position().x() - xForFrame(grabFrame)) / pixelsPerFrame_;
        move.targetEdgeFrame = grabFrame;
        move.targetFrame = clip->startFrame;
        move.durationFrames = clip->durationFrames;
        move.playheadFrame = currentFrame_;
        if (move.trimEdge) {
            const qint64 maximumFrame = std::numeric_limits<qint64>::max() / 4;
            if (leftTrim) {
                move.minEdgeFrame = clip->startFrame - availableTrimFrames(clip->trimStartFrame, clip->speed, clip->startFrame);
                move.maxEdgeFrame = clip->endFrame() - 1;
            } else {
                move.minEdgeFrame = clip->startFrame + 1;
                move.maxEdgeFrame = clip->endFrame() + availableTrimFrames(clip->trimEndFrame, clip->speed,
                    std::max<qint64>(0, maximumFrame - clip->endFrame()));
            }
        }
        clipMove_ = move;
        update();
        event->accept();
        return;
    }
    if (event->position().y() >= kRulerHeight) {
        if (event->position().x() < contentRight() &&
            event->position().y() >= kRulerHeight + kNewVideoTrackStripHeight - verticalScrollBar_->value()) {
            Marquee draft;
            draft.pressPosition = draft.lastPosition = event->position();
            draft.anchor = QPointF((event->position().x() - kHeaderWidth + scrollOffset_) / pixelsPerFrame_,
                                   event->position().y() + verticalScrollBar_->value());
            draft.baseline = selectedClipIds_;
            draft.additive = bool(event->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier));
            marquee_ = std::move(draft);
        }
        event->accept();
        return;
    }
    scrubbing_ = true;
    emit scrubStarted();
    updateScrub(event->position().x(), false);
    event->accept();
}

void TimelineWidget::mouseMoveEvent(QMouseEvent* event) {
    if (marquee_ && event->buttons().testFlag(Qt::LeftButton)) {
        updateMarquee(event->position());
        event->accept();
        return;
    }
    if (clipMove_ && event->buttons().testFlag(Qt::LeftButton)) {
        if (!clipMove_->dragging && (event->position() - clipMove_->pressPosition).manhattanLength()
                >= QApplication::startDragDistance()) {
            if (session_->editor().compatibility().level == heisenberg::CompatibilityLevel::Unsupported) {
                cancelInteraction();
                emit editFailed(tr("This timeline contains unsupported media or effects and cannot be edited."));
                event->accept();
                return;
            }
            clipMove_->dragging = true;
        }
        if (clipMove_->dragging) updateClipMove(event->position(), event->modifiers());
        event->accept();
        return;
    }
    if (scrubbing_) {
        updateScrub(event->position().x(), false);
        event->accept();
        return;
    }
    const std::string hovered = clipAt(event->position());
    if (hovered != hoveredClipId_) {
        hoveredClipId_ = hovered;
        update();
    }
    const auto* hoveredClip = timeline() ? timeline()->clipById(hovered) : nullptr;
    setCursor(hoveredClip ? (selectedClipIds_.size() <= 1 && trimEdgeAt(*hoveredClip, event->position().x()) ? Qt::SizeHorCursor : Qt::OpenHandCursor) :
        event->position().x() >= kHeaderWidth ? Qt::PointingHandCursor : Qt::ArrowCursor);
}

void TimelineWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (marquee_ && event->button() == Qt::LeftButton) {
        updateMarquee(event->position());
        const auto draft = *marquee_;
        marquee_.reset();
        if (draft.dragging) setSelection(draft.hits, selectedClipId_);
        else if (!draft.additive) {
            setSelectedClipId({});
            emit scrubStarted();
            updateScrub(event->position().x(), false);
            updateScrub(event->position().x(), true);
        }
        update();
        event->accept();
        return;
    }
    if (clipMove_ && event->button() == Qt::LeftButton) {
        if (clipMove_->dragging) updateClipMove(event->position(), event->modifiers());
        const auto move = *clipMove_;
        cancelInteraction();
        const bool changed = move.trimEdge
            ? move.targetEdgeFrame != (*move.trimEdge == heisenberg::TrimEdge::Left ? move.originalStartFrame : move.originalEndFrame)
            : move.createNewTrack || std::any_of(move.members.begin(), move.members.end(), [](const auto& member) {
                return member.startFrame != member.targetFrame || member.trackId != member.targetTrackId;
            });
        if (move.dragging && move.validDrop && changed) {
            mutate([this, move](heisenberg::ui::EditorSession& active) {
                const auto* current = active.editor().activeTimeline();
                if (!current || current->id != move.timelineId) return heisenberg::EditResult{};
                if (!move.trimEdge && move.members.size() > 1) {
                    std::vector<heisenberg::ClipMoveRequest> requests;
                    std::unordered_set<std::string> ids;
                    for (const auto& member : move.members) {
                        const auto* clip = current->clipById(member.clipId);
                        const auto* owner = current->trackForClip(member.clipId);
                        if (!clip || !owner || owner->id != member.trackId || clip->startFrame != member.startFrame ||
                            clip->durationFrames != member.durationFrames) return heisenberg::EditResult{};
                        requests.push_back({member.clipId, member.targetTrackId, member.targetFrame});
                        ids.insert(member.clipId);
                    }
                    auto result = active.moveClips(requests, move.createNewTrack);
                    if (session_ == &active && result.changed()) setSelection(std::move(ids), move.clipId);
                    return result;
                }
                const auto* clip = current->clipById(move.clipId);
                const auto* source = current ? current->trackForClip(move.clipId) : nullptr;
                const auto* destination = current ? current->trackById(move.targetTrackId) : nullptr;
                if (!clip || !source) return heisenberg::EditResult{};
                if (move.createNewTrack) {
                    if (move.trimEdge || source->type != heisenberg::ClipType::Video ||
                        clip->mediaType != heisenberg::ClipType::Video || !move.targetTrackId.empty()) return heisenberg::EditResult{};
                } else if (!destination || source->type != destination->type ||
                           (move.trimEdge && source->id != destination->id)) return heisenberg::EditResult{};
                const qint64 delta = move.trimEdge ? move.targetEdgeFrame -
                    (*move.trimEdge == heisenberg::TrimEdge::Left ? clip->startFrame : clip->endFrame()) : 0;
                auto result = move.trimEdge
                    ? active.trimClip(QString::fromStdString(move.clipId), *move.trimEdge, delta)
                    : active.moveClip(QString::fromStdString(move.clipId), move.targetFrame,
                                      QString::fromStdString(move.targetTrackId), move.createNewTrack);
                if (session_ == &active && result.changed()) setSelectedClipId(move.clipId);
                return result;
            });
        } else if (!move.dragging) setSelectedClipId(move.clipId);
        event->accept();
        return;
    }
    if (scrubbing_ && event->button() == Qt::LeftButton) {
        scrubbing_ = false;
        updateScrub(event->position().x(), true);
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void TimelineWidget::wheelEvent(QWheelEvent* event) {
    const QPoint angle = event->angleDelta(), pixel = event->pixelDelta();
    if (event->modifiers().testFlag(Qt::ControlModifier)) {
        const double steps = !angle.isNull() ? static_cast<double>(angle.y() ? angle.y() : angle.x()) / 120.0
                                            : static_cast<double>(pixel.y()) / 80.0;
        setZoom(pixelsPerFrame_ * std::pow(1.25, steps), event->position().x());
    } else if (event->modifiers().testFlag(Qt::ShiftModifier) || angle.x() || pixel.x()) {
        const double distance = !pixel.isNull() ? (pixel.x() ? pixel.x() : pixel.y())
            : static_cast<double>(angle.x() ? angle.x() : angle.y()) / 120.0 * 80.0;
        scrollOffset_ -= distance;
        clampView();
        update();
    } else {
        const double distance = !pixel.isNull() ? pixel.y() : static_cast<double>(angle.y()) / 120.0 * 80.0;
        verticalScrollBar_->setValue(verticalScrollBar_->value() - static_cast<int>(std::round(distance)));
    }
    if (clipMove_ && clipMove_->dragging)
        updateClipMove(clipMove_->lastPosition, clipMove_->modifiers);
    if (marquee_ && marquee_->dragging) updateMarquee(marquee_->lastPosition);
    event->accept();
}

void TimelineWidget::leaveEvent(QEvent* event) {
    hoveredClipId_.clear();
    if (!clipMove_) unsetCursor();
    update();
    QWidget::leaveEvent(event);
}

void TimelineWidget::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_A && event->modifiers() == Qt::ControlModifier) {
        cancelInteraction();
        std::unordered_set<std::string> ids;
        if (const auto* active = timeline()) for (const auto& track : active->tracks)
            for (const auto& clip : track.clips) ids.insert(clip.id);
        setSelection(std::move(ids), selectedClipId_);
        event->accept();
        return;
    }
    if ((event->key() == Qt::Key_M || event->key() == Qt::Key_H) && event->modifiers() == Qt::NoModifier) {
        if (!event->isAutoRepeat() && !selectedTrackId_.empty()) toggleTrack(selectedTrackId_, event->key() == Qt::Key_H);
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Delete && event->modifiers() == Qt::ShiftModifier) {
        if (!event->isAutoRepeat()) rippleDeleteSelectedClip();
        event->accept();
        return;
    }
    if ((event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) &&
        !(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
        if (!event->isAutoRepeat()) removeSelectedClip();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_S && !(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
        if (!event->isAutoRepeat()) splitSelectedClip();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Escape) {
        if (!cancelInteraction()) setSelectedClipId({});
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Shift && clipMove_ && clipMove_->dragging) {
        updateClipMove(clipMove_->lastPosition, event->modifiers() | Qt::ShiftModifier);
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void TimelineWidget::keyReleaseEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Shift && clipMove_ && clipMove_->dragging) {
        updateClipMove(clipMove_->lastPosition, event->modifiers() & ~Qt::ShiftModifier);
        event->accept();
        return;
    }
    QWidget::keyReleaseEvent(event);
}

bool TimelineWidget::event(QEvent* event) {
    if (event->type() == QEvent::ShortcutOverride && hasFocus()) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape && (clipMove_ || marquee_ || !selectedClipIds_.empty())) {
            event->accept();
            return true;
        }
    }
    if (event->type() == QEvent::WindowDeactivate || event->type() == QEvent::Hide)
        cancelInteraction();
    return QWidget::event(event);
}

void TimelineWidget::updateClipMove(const QPointF& point, Qt::KeyboardModifiers modifiers) {
    if (!clipMove_) return;
    auto& move = *clipMove_;
    if (!move.trimEdge) {
        move.lastPosition = point;
        move.modifiers = modifiers;
        const bool canCreateTrack = std::all_of(move.members.begin(), move.members.end(), [&](const auto& member) {
            return member.type == heisenberg::ClipType::Video && member.trackId == move.trackId;
        });
        const auto destination = dropDestination(point, move.type, false, canCreateTrack);
        move.createNewTrack = destination && destination->createNewTrack;
        move.targetTrackId = destination ? destination->trackId : move.trackId;
        move.validDrop = destination.has_value();
        std::vector<std::string> tracks;
        if (const auto* active = timeline()) for (const auto& track : active->tracks)
            if (track.type == move.type) tracks.push_back(track.id);
        const auto source = std::find(tracks.begin(), tracks.end(), move.trackId);
        const auto target = std::find(tracks.begin(), tracks.end(), move.targetTrackId);
        int trackDelta = 0;
        if (move.validDrop && !move.createNewTrack) {
            if (source == tracks.end() || target == tracks.end()) move.validDrop = false;
            else trackDelta = static_cast<int>(target - source);
        }
        qint64 earliest = move.originalStartFrame;
        qint64 latestEnd = move.originalEndFrame;
        std::unordered_set<std::string> movingIds;
        for (auto& member : move.members) {
            movingIds.insert(member.clipId);
            earliest = std::min(earliest, member.startFrame);
            latestEnd = std::max(latestEnd, member.startFrame + member.durationFrames);
            member.targetTrackId = member.trackId;
            if (move.createNewTrack) member.targetTrackId.clear();
            else if (member.type == move.type) {
                const auto owner = std::find(tracks.begin(), tracks.end(), member.trackId);
                const int index = static_cast<int>(owner - tracks.begin()) + trackDelta;
                if (owner == tracks.end() || index < 0 || index >= static_cast<int>(tracks.size())) move.validDrop = false;
                else member.targetTrackId = tracks[index];
            }
        }
        const qint64 minDelta = -earliest;
        const qint64 maxDelta = std::max<qint64>(0, std::numeric_limits<qint64>::max() / 4 - latestEnd);
        const double rawDelta = (point.x() - kHeaderWidth + scrollOffset_) / pixelsPerFrame_ -
            move.grabOffsetFrames - static_cast<double>(move.originalStartFrame);
        qint64 delta = static_cast<qint64>(std::round(std::clamp(rawDelta, double(minDelta), double(maxDelta))));
        move.sourceLimited = rawDelta < double(minDelta) || rawDelta > double(maxDelta);
        move.snapFrame.reset();
        if (move.validDrop && snappingEnabled_ && !modifiers.testFlag(Qt::ShiftModifier)) {
            double bestPixels = 8.0 + 0.001;
            auto consider = [&](qint64 anchor) {
                for (const auto& member : move.members) for (const qint64 edge : {member.startFrame, member.startFrame + member.durationFrames}) {
                    const qint64 candidate = anchor - edge;
                    if (candidate < minDelta || candidate > maxDelta) continue;
                    const double distance = std::abs(double(candidate) - rawDelta) * pixelsPerFrame_;
                    if (distance < bestPixels) {
                        bestPixels = distance;
                        delta = candidate;
                        move.snapFrame = anchor;
                        move.sourceLimited = false;
                    }
                }
            };
            consider(0);
            consider(move.playheadFrame);
            if (const auto* active = timeline()) for (const auto& track : active->tracks)
                for (const auto& clip : track.clips) if (!movingIds.contains(clip.id)) {
                    consider(clip.startFrame);
                    consider(clip.endFrame());
                }
        }
        for (auto& member : move.members) member.targetFrame = member.startFrame + delta;
        move.targetFrame = move.originalStartFrame + delta;
        setCursor(move.validDrop ? Qt::ClosedHandCursor : Qt::ForbiddenCursor);
        update();
        return;
    }
    move.lastPosition = point;
    move.modifiers = modifiers;
    const auto destination = dropDestination(point, move.type, false, !move.trimEdge);
    move.createNewTrack = destination && destination->createNewTrack;
    move.targetTrackId = destination ? destination->trackId : move.trackId;
    move.validDrop = destination && (!move.trimEdge || destination->trackId == move.trackId);
    const double raw = std::clamp((point.x() - kHeaderWidth + scrollOffset_) / pixelsPerFrame_ - move.grabOffsetFrames,
        0.0, static_cast<double>(std::numeric_limits<qint64>::max() / 4));
    const qint64 rounded = static_cast<qint64>(std::round(raw));
    move.sourceLimited = false;
    if (move.trimEdge) {
        move.targetEdgeFrame = std::clamp(rounded, move.minEdgeFrame, move.maxEdgeFrame);
        move.sourceLimited = move.targetEdgeFrame != rounded;
    } else move.targetFrame = rounded;
    move.snapFrame.reset();
    if (move.validDrop && snappingEnabled_ && !modifiers.testFlag(Qt::ShiftModifier)) {
        // Moves snap either edge; trims snap only the edited edge. Never snap to self.
        double bestPixels = 8.0 + 0.001;
        auto consider = [&](qint64 anchor) {
            const int edgeCount = move.trimEdge ? 1 : 2;
            for (int index = 0; index < edgeCount; ++index) {
                const qint64 edge = index == 0 ? 0 : move.durationFrames;
                if (anchor < edge) continue;
                const qint64 candidate = anchor - edge;
                if (move.trimEdge && (candidate < move.minEdgeFrame || candidate > move.maxEdgeFrame)) continue;
                const double distance = std::abs(static_cast<double>(candidate) - raw) * pixelsPerFrame_;
                if (distance < bestPixels) {
                    bestPixels = distance;
                    if (move.trimEdge) move.targetEdgeFrame = candidate;
                    else move.targetFrame = candidate;
                    move.snapFrame = anchor;
                    move.sourceLimited = false;
                }
            }
        };
        consider(0);
        consider(move.playheadFrame);
        if (const auto* active = timeline()) for (const auto& track : active->tracks)
            for (const auto& clip : track.clips) if (clip.id != move.clipId) {
                consider(clip.startFrame);
                consider(clip.endFrame());
            }
    }
    if (move.trimEdge) {
        move.targetFrame = *move.trimEdge == heisenberg::TrimEdge::Left ? move.targetEdgeFrame : move.originalStartFrame;
        move.durationFrames = (*move.trimEdge == heisenberg::TrimEdge::Left ? move.originalEndFrame : move.targetEdgeFrame) - move.targetFrame;
    }
    setCursor(move.validDrop ? (move.trimEdge ? Qt::SizeHorCursor : Qt::ClosedHandCursor) : Qt::ForbiddenCursor);
    update();
}

QString TimelineWidget::mediaFromMime(const QMimeData* mime) const {
    if (!session_ || !mime->hasFormat(heisenberg::ui::kMediaMimeType)) return {};
    const auto data = QJsonDocument::fromJson(mime->data(heisenberg::ui::kMediaMimeType)).object();
    if (data.value("session").toString() != session_->token()) return {};
    const QString ref = data.value("mediaRef").toString();
    const auto* entry = session_->editor().manifest().find(ref.toStdString());
    if (!entry || (entry->type != heisenberg::ClipType::Video && entry->type != heisenberg::ClipType::Audio) ||
        session_->editor().resolver().isMissing(entry->id)) return {};
    return ref;
}

qint64 TimelineWidget::dropFrameAtX(double x) const {
    // A placement can extend the timeline, unlike playback scrubbing.
    const double frame = std::floor((x - kHeaderWidth + scrollOffset_) / pixelsPerFrame_);
    return static_cast<qint64>(std::clamp(frame, 0.0,
        static_cast<double>(std::numeric_limits<qint64>::max() / 4)));
}

std::optional<TimelineWidget::DropDestination> TimelineWidget::dropDestination(
        const QPointF& point, heisenberg::ClipType type, bool allowPlaceholder, bool allowNewTrack) const {
    if (!session_ || !timeline() || point.x() < kHeaderWidth || point.x() >= contentRight() ||
        point.y() < kRulerHeight || point.y() >= height()) return std::nullopt;
    if (session_->editor().compatibility().level == heisenberg::CompatibilityLevel::Unsupported) return std::nullopt;
    const double stripTop = kRulerHeight - verticalScrollBar_->value();
    if (point.y() >= stripTop && point.y() < stripTop + kNewVideoTrackStripHeight) {
        if (allowNewTrack && type == heisenberg::ClipType::Video)
            return DropDestination{{}, true, stripTop, kNewVideoTrackStripHeight};
        return std::nullopt;
    }
    const auto lane = laneAt(point);
    if (!lane || lane->type != type || (!lane->track && !allowPlaceholder)) return std::nullopt;
    return DropDestination{lane->track ? lane->track->id : std::string{}, false, lane->top, lane->height};
}

void TimelineWidget::dragEnterEvent(QDragEnterEvent* event) {
    if (!mediaFromMime(event->mimeData()).isEmpty()) {
        const auto report = session_->editor().compatibility();
        if (report.level == heisenberg::CompatibilityLevel::Unsupported) {
            QStringList messages;
            for (const auto& message : report.messages)
                messages << QString::fromUtf8(message.c_str());
            emit editFailed(messages.join('\n'));
            event->ignore();
            return;
        }
        event->setDropAction(Qt::CopyAction);
        event->accept();
    }
}

void TimelineWidget::dragMoveEvent(QDragMoveEvent* event) {
    const QString ref = mediaFromMime(event->mimeData());
    const auto* entry = session_ ? session_->editor().manifest().find(ref.toStdString()) : nullptr;
    const auto destination = entry ? dropDestination(event->position(), entry->type, true, true) : std::nullopt;
    qint64 trim = 0, duration = 0;
    if (!ref.isEmpty() && destination && session_->sourcePlacementRange(ref, trim, duration)) {
        draggedMedia_ = ref;
        dropFrame_ = dropFrameAtX(event->position().x());
        dropDestination_ = destination;
        event->setDropAction(Qt::CopyAction);
        event->accept();
    } else {
        draggedMedia_.clear();
        dropDestination_.reset();
        event->ignore();
    }
    update();
}

void TimelineWidget::dragLeaveEvent(QDragLeaveEvent* event) {
    draggedMedia_.clear();
    dropDestination_.reset();
    update();
    event->accept();
}

void TimelineWidget::dropEvent(QDropEvent* event) {
    draggedMedia_.clear();
    dropDestination_.reset();
    const QString ref = mediaFromMime(event->mimeData());
    const auto* entry = session_ ? session_->editor().manifest().find(ref.toStdString()) : nullptr;
    const auto destination = entry ? dropDestination(event->position(), entry->type, true, true) : std::nullopt;
    qint64 initialTrim = 0, initialDuration = 0;
    if (editInProgress_ || ref.isEmpty() || !destination ||
        !session_->sourcePlacementRange(ref, initialTrim, initialDuration)) {
        event->ignore();
        update();
        return;
    }
    // Keep only values across editRequested: flushing a grade replaces model storage.
    const QString targetTrackId = QString::fromStdString(destination->trackId);
    const bool createNewTrack = destination->createNewTrack;
    const qint64 frame = dropFrameAtX(event->position().x());
    const bool insert = insertMode_;
    const QPointer<heisenberg::ui::EditorSession> session = session_;
    cancelInteraction();
    QScopedValueRollback<bool> editing(editInProgress_, true);
    emit editRequested();
    if (!session || session_ != session.data()) {
        event->ignore();
        return;
    }
    qint64 trim = 0, duration = 0;
    QString error;
    if (!session->sourcePlacementRange(ref, trim, duration, &error)) {
        if (!error.isEmpty()) emit editFailed(error);
        event->ignore();
        update();
        return;
    }
    const auto result = insert ? session->insertMedia(ref, frame, targetTrackId, createNewTrack)
                               : session->placeMedia(ref, frame, targetTrackId, createNewTrack);
    if (result.changed()) {
        if (session && session_ == session.data() && !result.affectedIds.empty())
            setSelectedClipId(result.affectedIds.back());
        event->setDropAction(Qt::CopyAction);
        event->accept();
    } else {
        emit editFailed(QString::fromUtf8(result.message.c_str()));
        event->ignore();
    }
    update();
}
