#include "MainWidget/TimelineWidget.hpp"
#include "Backend/EditorSession.hpp"

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
#include <QMenu>
#include <QPushButton>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace {
constexpr double kHeaderWidth = 160.0;
constexpr double kRulerHeight = 30.0;
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
}

void TimelineWidget::setSession(heisenberg::ui::EditorSession* session) {
    if (session_) disconnect(session_, nullptr, this, nullptr);
    cancelInteraction();
    selectedClipId_.clear();
    session_ = session;
    if (!session_) { refreshProject(); return; }
    connect(session_, &heisenberg::ui::EditorSession::projectChanged, this, [this]() {
        cancelInteraction();
        selectedClipId_.clear();
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
    connect(session_, &heisenberg::ui::EditorSession::timelineChanged, this, [this](qint64 frame) {
        cancelInteraction();
        refreshProject();
        if (frame >= 0) setCurrentFrame(frame);
    });
    refreshProject();
}

void TimelineWidget::refreshProject() {
    draggedMedia_.clear();
    dropLane_.reset();
    mediaNames_.clear();
    missingMedia_.clear();
    if (session_) {
        for (const auto& asset : session_->editor().assets()) {
            mediaNames_.emplace(asset.id, asset.name);
            if (asset.missing) missingMedia_.insert(asset.id);
        }
    }
    playbackFrameCount_ = timeline() ? timeline()->totalFrames() : 0;
    if (!timeline() || !timeline()->clipById(selectedClipId_)) selectedClipId_.clear();
    currentFrame_ = std::clamp<qint64>(currentFrame_, 0, std::max<qint64>(0, contentFrameCount() - 1));
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
    const bool active = clipMove_.has_value();
    clipMove_.reset();
    unsetCursor();
    update();
    return active;
}

void TimelineWidget::setSnappingEnabled(bool enabled) {
    snappingEnabled_ = enabled;
    if (clipMove_ && clipMove_->dragging)
        updateClipMove(clipMove_->lastPosition, clipMove_->modifiers);
}

void TimelineWidget::splitSelectedClip() {
    cancelInteraction();
    if (!session_ || selectedClipId_.empty()) {
        emit editFailed(tr("Select a clip and place the playhead inside it before splitting."));
        return;
    }
    const auto result = session_->splitClip(QString::fromStdString(selectedClipId_), currentFrame_);
    if (!result.changed() && !result.message.empty()) emit editFailed(QString::fromUtf8(result.message.c_str()));
}

void TimelineWidget::removeSelectedClip() {
    cancelInteraction();
    if (!session_ || selectedClipId_.empty()) return;
    const auto result = session_->removeClips({QString::fromStdString(selectedClipId_)});
    if (!result.changed() && !result.message.empty()) emit editFailed(QString::fromUtf8(result.message.c_str()));
}

void TimelineWidget::editSelectedClipAudio() {
    cancelInteraction();
    const auto* clip = timeline() ? timeline()->clipById(selectedClipId_) : nullptr;
    if (!session_ || !clip) return;
    if (session_->editor().compatibility().level == heisenberg::CompatibilityLevel::Unsupported) {
        emit editFailed(tr("Audio editing supports at most one video track and one audio track."));
        return;
    }
    const QString id = QString::fromStdString(clip->id);
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
    if (dialog.exec() != QDialog::Accepted) return;
    const auto result = session_->setClipAudio(id, gain->value() / 100.0,
        std::llround(fadeIn->value() * fps), std::llround(fadeOut->value() * fps));
    if (!result.changed() && !result.message.empty()) emit editFailed(QString::fromUtf8(result.message.c_str()));
}

void TimelineWidget::editSelectedClipGrade() {
    cancelInteraction();
    const auto* clip = timeline() ? timeline()->clipById(selectedClipId_) : nullptr;
    if (!session_ || !clip || clip->mediaType != heisenberg::ClipType::Video) return;
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
    if (dialog.exec() != QDialog::Accepted) return;
    bool changed = false;
    for (size_t i = 0; i < fields.size(); ++i) changed |= fields[i]->value() != original[i];
    if (!changed) return;
    if (index == effects.size()) effects.push_back({{"id", "basic-grade"}, {"type", "video.basicGrade"}, {"enabled", true}, {"params", heisenberg::JsonValue::object()}});
    auto& params = effects[index]["params"];
    if (!params.is_object()) params = heisenberg::JsonValue::object();
    for (size_t i = 0; i < fields.size(); ++i) {
        if (fields[i]->value() == original[i]) continue;
        auto& parameter = params[names[i]];
        if (!parameter.is_object()) parameter = heisenberg::JsonValue::object();
        parameter["value"] = fields[i]->value();
        parameter.erase("track");
    }
    const auto result = session_->setClipEffects(id, std::move(effects));
    if (!result.changed() && !result.message.empty()) emit editFailed(QString::fromUtf8(result.message.c_str()));
}

void TimelineWidget::contextMenuEvent(QContextMenuEvent* event) {
    const auto hit = clipAt(event->pos());
    if (hit.empty()) { QWidget::contextMenuEvent(event); return; }
    cancelInteraction();
    selectedClipId_ = hit;
    setFocus(Qt::MouseFocusReason);
    update();
    QMenu menu(this);
    menu.setObjectName("timelineClipMenu");
    auto* audio = menu.addAction(tr("Audio Settings..."));
    audio->setObjectName("clipAudioSettings");
    auto* grade = menu.addAction(tr("Basic Grade..."));
    grade->setObjectName("clipBasicGrade");
    const auto* selected = timeline() ? timeline()->clipById(selectedClipId_) : nullptr;
    grade->setEnabled(selected && selected->mediaType == heisenberg::ClipType::Video);
    auto* remove = menu.addAction(tr("Delete Clip"));
    const auto* chosen = menu.exec(event->globalPos());
    if (chosen == audio) editSelectedClipAudio();
    else if (chosen == grade) editSelectedClipGrade();
    else if (chosen == remove) removeSelectedClip();
    event->accept();
}

void TimelineWidget::setPlaying(bool playing) {
    if (playing_ == playing) return;
    playing_ = playing;
    update();
}

void TimelineWidget::fitToContent() {
    const qint64 frames = contentFrameCount();
    const double available = std::max(1.0, width() - kHeaderWidth - 24.0);
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
    double laneHeight = kRulerHeight + 24.0;
    for (const auto& lane : lanes()) laneHeight += lane.height;
    return QSize(480, static_cast<int>(std::ceil(laneHeight)));
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
    const double viewport = std::max(1.0, width() - kHeaderWidth);
    return std::max(0.0, static_cast<double>(contentFrameCount()) * pixelsPerFrame_ - viewport + 24.0);
}

void TimelineWidget::clampView() {
    pixelsPerFrame_ = std::clamp(pixelsPerFrame_, kMinZoom, kMaxZoom);
    scrollOffset_ = std::clamp(scrollOffset_, 0.0, maximumScrollOffset());
}

void TimelineWidget::setZoom(double zoom, double anchorX) {
    const double boundedAnchor = std::clamp(anchorX, kHeaderWidth, static_cast<double>(width()));
    const double anchorFrame = (boundedAnchor - kHeaderWidth + scrollOffset_) / pixelsPerFrame_;
    pixelsPerFrame_ = std::clamp(zoom, kMinZoom, kMaxZoom);
    scrollOffset_ = anchorFrame * pixelsPerFrame_ - (boundedAnchor - kHeaderWidth);
    clampView();
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

    const auto* active = timeline();
    double y = kRulerHeight;
    for (const auto& lane : lanes()) {
        const double trackHeight = lane.height;
        y = lane.top;
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
            painter.fillRect(headerRect, kSurface);
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
            painter.drawText(QRectF(12, y + 5, 105, 17), Qt::AlignLeft | Qt::AlignVCenter, trackName);
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
            painter.setClipRect(laneRect);
            for (const auto& clip : track.clips) {
                painter.setOpacity(clipMove_ && clipMove_->dragging && clipMove_->clipId == clip.id ? 0.35 : 1.0);
                const double clipX = xForFrame(clip.startFrame);
                const double clipWidth = std::max(1.0, clip.durationFrames * pixelsPerFrame_);
                QRectF clipRect(clipX, y + 3, clipWidth, trackHeight - 6);
                if (clipRect.right() < kHeaderWidth || clipRect.left() > width()) continue;
                const bool missing = missingMedia_.contains(clip.mediaRef);
                const QColor color = clipColor(clip.sourceClipType, missing);
                QPainterPath path;
                path.addRoundedRect(clipRect, 4, 4);
                painter.fillPath(path, color);
                painter.fillRect(QRectF(clipRect.left(), clipRect.top(), clipRect.width(),
                                        std::min(16.0, clipRect.height())), color.lighter(112));
                const bool selected = selectedClipId_ == clip.id;
                const bool hovered = hoveredClipId_ == clip.id;
                painter.setPen(QPen(selected ? QColor(255, 255, 255) : QColor(0, 0, 0, hovered ? 210 : 150),
                                    selected ? 1.5 : 1.0));
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

    if (!draggedMedia_.isEmpty() && dropLane_ && session_ && active) {
        const auto* entry = session_->editor().manifest().find(draggedMedia_.toStdString());
        if (entry) {
            const double ghostWidth = std::max(2.0, entry->duration * active->fps * pixelsPerFrame_);
            painter.save();
            painter.setClipRect(QRectF(kHeaderWidth, 0, width() - kHeaderWidth, height()));
            painter.setPen(QPen(QColor(255, 255, 255, 220), 1, Qt::DashLine));
            QColor ghostColor = clipColor(entry->type, false);
            ghostColor.setAlpha(95);
            painter.setBrush(ghostColor);
            painter.drawRoundedRect(QRectF(xForFrame(dropFrame_), dropLane_->top + 3,
                                           ghostWidth, dropLane_->height - 6), 4, 4);
            painter.setPen(kPrimaryText);
            painter.drawText(QPointF(xForFrame(dropFrame_) + 6, dropLane_->top + dropLane_->height + 18),
                             tr("Overwrite at %1").arg(formatTimecode(dropFrame_)));
            painter.restore();
        }
    }

    if (clipMove_ && clipMove_->dragging && active && !active->tracks.empty()) {
        const auto& move = *clipMove_;
        const auto lane = laneForTrack(move.trackId);
        const double trackHeight = lane ? lane->height : kDefaultTrackHeight;
        const double trackTop = lane ? lane->top : kRulerHeight;
        painter.save();
        painter.setClipRect(QRectF(kHeaderWidth, 0, width() - kHeaderWidth, height()));
        const QColor accent = move.validDrop ? QColor(112, 199, 238) : QColor(229, 122, 122);
        painter.setPen(QPen(accent, 1.5, Qt::DashLine));
        painter.setBrush(QColor(accent.red(), accent.green(), accent.blue(), 95));
        const QRectF ghost(xForFrame(move.targetFrame), trackTop + 3,
                           std::max(2.0, move.durationFrames * pixelsPerFrame_), trackHeight - 6);
        painter.drawRoundedRect(ghost, 4, 4);
        if (move.snapFrame && move.validDrop) {
            const double snapX = xForFrame(*move.snapFrame);
            painter.setPen(QPen(QColor(245, 199, 98), 1.5));
            painter.drawLine(QPointF(snapX, 0), QPointF(snapX, height()));
        }
        painter.setPen(accent);
        const double labelX = std::clamp(ghost.left() + 6, kHeaderWidth + 6, std::max(kHeaderWidth + 6, width() - 230.0));
        QString label = tr("Release in the original lane to commit");
        if (move.validDrop) {
            label = move.trimEdge
                ? tr("%1  %2  /  %3 frames").arg(*move.trimEdge == heisenberg::TrimEdge::Left ? tr("Trim In") : tr("Trim Out"))
                      .arg(formatTimecode(move.targetEdgeFrame)).arg(move.durationFrames)
                : tr("Move / overwrite at %1").arg(formatTimecode(move.targetFrame));
            if (move.snapFrame) label += tr("  [Snap]");
            if (move.sourceLimited) label += tr("  [Limit]");
        }
        if (move.trimEdge) {
            painter.setPen(QPen(accent, 3));
            painter.drawLine(QPointF(xForFrame(move.targetEdgeFrame), ghost.top()),
                             QPointF(xForFrame(move.targetEdgeFrame), ghost.bottom()));
        }
        painter.setPen(accent);
        painter.drawText(QPointF(labelX, trackTop + trackHeight + 18), label);
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
    double top = kRulerHeight;
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
    if (point.y() >= height()) return std::nullopt;
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
    if (right <= width() && right - x <= handleWidth) return heisenberg::TrimEdge::Right;
    return std::nullopt;
}

void TimelineWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    if (event->position().x() < kHeaderWidth) {
        const auto lane = laneAt(event->position());
        if (session_ && lane && lane->track) {
            setFocus(Qt::MouseFocusReason);
            cancelInteraction();
            const auto result = session_->setTrackMuted(QString::fromStdString(lane->track->id), !lane->track->muted);
            if (!result.changed() && !result.message.empty()) emit editFailed(QString::fromUtf8(result.message.c_str()));
            event->accept();
        } else QWidget::mousePressEvent(event);
        return;
    }
    setFocus(Qt::MouseFocusReason);
    emit timelineActivated();
    cancelInteraction();
    const auto hit = clipAt(event->position());
    if (!hit.empty()) {
        selectedClipId_ = hit;
        const auto* clip = timeline()->clipById(hit);
        ClipMove move;
        move.clipId = hit;
        move.trackId = timeline()->trackForClip(hit)->id;
        move.pressPosition = move.lastPosition = event->position();
        move.trimEdge = trimEdgeAt(*clip, event->position().x());
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
    if (event->position().y() >= kRulerHeight) selectedClipId_.clear();
    scrubbing_ = true;
    emit scrubStarted();
    updateScrub(event->position().x(), false);
    event->accept();
}

void TimelineWidget::mouseMoveEvent(QMouseEvent* event) {
    if (clipMove_ && event->buttons().testFlag(Qt::LeftButton)) {
        if (!clipMove_->dragging && (event->position() - clipMove_->pressPosition).manhattanLength()
                >= QApplication::startDragDistance()) {
            if (session_->editor().compatibility().level == heisenberg::CompatibilityLevel::Unsupported) {
                cancelInteraction();
                emit editFailed(tr("Editing supports at most one video track and one audio track."));
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
    setCursor(hoveredClip ? (trimEdgeAt(*hoveredClip, event->position().x()) ? Qt::SizeHorCursor : Qt::OpenHandCursor) :
        event->position().x() >= kHeaderWidth ? Qt::PointingHandCursor : Qt::ArrowCursor);
}

void TimelineWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (clipMove_ && event->button() == Qt::LeftButton) {
        if (clipMove_->dragging) updateClipMove(event->position(), event->modifiers());
        const auto move = *clipMove_;
        cancelInteraction();
        if (move.dragging && move.validDrop) {
            const auto result = move.trimEdge
                ? session_->trimClip(QString::fromStdString(move.clipId), *move.trimEdge,
                    move.targetEdgeFrame - (*move.trimEdge == heisenberg::TrimEdge::Left ? move.originalStartFrame : move.originalEndFrame))
                : session_->moveClip(QString::fromStdString(move.clipId), move.targetFrame);
            if (result.status == heisenberg::EditStatus::Invalid || result.status == heisenberg::EditStatus::Unsupported)
                emit editFailed(QString::fromUtf8(result.message.c_str()));
        }
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
    const QPoint delta = event->angleDelta();
    if (event->modifiers().testFlag(Qt::ControlModifier)) {
        const double steps = static_cast<double>(delta.y()) / 120.0;
        setZoom(pixelsPerFrame_ * std::pow(1.25, steps), event->position().x());
    } else {
        const int wheel = std::abs(delta.x()) > std::abs(delta.y()) ? delta.x() : delta.y();
        scrollOffset_ -= static_cast<double>(wheel) / 120.0 * 80.0;
        clampView();
        update();
    }
    if (clipMove_ && clipMove_->dragging)
        updateClipMove(clipMove_->lastPosition, clipMove_->modifiers);
    event->accept();
}

void TimelineWidget::leaveEvent(QEvent* event) {
    hoveredClipId_.clear();
    if (!clipMove_) unsetCursor();
    update();
    QWidget::leaveEvent(event);
}

void TimelineWidget::keyPressEvent(QKeyEvent* event) {
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
    if (event->key() == Qt::Key_Escape && cancelInteraction()) { event->accept(); return; }
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
    if (event->type() == QEvent::WindowDeactivate || event->type() == QEvent::Hide)
        cancelInteraction();
    return QWidget::event(event);
}

void TimelineWidget::updateClipMove(const QPointF& point, Qt::KeyboardModifiers modifiers) {
    if (!clipMove_) return;
    auto& move = *clipMove_;
    move.lastPosition = point;
    move.modifiers = modifiers;
    const auto lane = laneAt(point);
    move.validDrop = canDropAt(point) && lane && lane->track && lane->track->id == move.trackId;
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

bool TimelineWidget::canDropAt(const QPointF& point, const QString& mediaRef) const {
    if (!session_ || !timeline() || point.x() < kHeaderWidth || point.x() >= width()) return false;
    if (session_->editor().compatibility().level == heisenberg::CompatibilityLevel::Unsupported) return false;
    const auto lane = laneAt(point);
    if (!lane) return false;
    if (mediaRef.isEmpty()) return lane->track != nullptr;
    const auto* entry = session_->editor().manifest().find(mediaRef.toStdString());
    return entry && entry->type == lane->type;
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
    if (!ref.isEmpty() && canDropAt(event->position(), ref)) {
        draggedMedia_ = ref;
        dropFrame_ = dropFrameAtX(event->position().x());
        dropLane_ = laneAt(event->position());
        event->setDropAction(Qt::CopyAction);
        event->accept();
    } else {
        draggedMedia_.clear();
        event->ignore();
    }
    update();
}

void TimelineWidget::dragLeaveEvent(QDragLeaveEvent* event) {
    draggedMedia_.clear();
    dropLane_.reset();
    update();
    event->accept();
}

void TimelineWidget::dropEvent(QDropEvent* event) {
    draggedMedia_.clear();
    const QString ref = mediaFromMime(event->mimeData());
    if (!ref.isEmpty() && canDropAt(event->position(), ref)) {
        const auto lane = laneAt(event->position());
        const QString targetTrackId = lane->track ? QString::fromStdString(lane->track->id) : QString();
        const auto result = session_->placeMedia(ref, dropFrameAtX(event->position().x()), targetTrackId);
        if (result.changed()) {
            if (!result.affectedIds.empty()) selectedClipId_ = result.affectedIds.back();
            event->setDropAction(Qt::CopyAction);
            event->accept();
        } else {
            emit editFailed(QString::fromUtf8(result.message.c_str()));
            event->ignore();
        }
    } else event->ignore();
    update();
}
