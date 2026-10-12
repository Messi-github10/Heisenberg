#include "MainWidget/TimelineWidget.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QStringList>
#include <QWheelEvent>

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

const QColor kBase(24, 25, 28);
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
}

TimelineWidget::TimelineWidget(QWidget* parent)
    : QWidget(parent) {
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    setAttribute(Qt::WA_OpaquePaintEvent);
}

bool TimelineWidget::loadPackage(const QString& packageDirectory, QString* error) {
    const QDir directory(packageDirectory);
    QFile projectFile(directory.filePath("project.json"));
    if (!projectFile.open(QIODevice::ReadOnly)) {
        if (error) *error = tr("Cannot open %1").arg(projectFile.fileName());
        return false;
    }

    heisenberg::ProjectFile project;
    std::string codecError;
    const QByteArray projectBytes = projectFile.readAll();
    if (!heisenberg::ProjectCodec::decode(
            std::string(projectBytes.constData(), static_cast<size_t>(projectBytes.size())),
            project, &codecError)) {
        if (error) *error = QString::fromUtf8(codecError.c_str());
        return false;
    }

    heisenberg::MediaManifest manifest;
    QFile manifestFile(directory.filePath("media.json"));
    if (!manifestFile.open(QIODevice::ReadOnly)) {
        if (error) *error = tr("Cannot open %1").arg(manifestFile.fileName());
        return false;
    }
    const QByteArray manifestBytes = manifestFile.readAll();
    if (!manifest.loadFromJson(
            std::string(manifestBytes.constData(), static_cast<size_t>(manifestBytes.size())),
            &codecError)) {
        if (error) *error = QString::fromUtf8(codecError.c_str());
        return false;
    }

    project_ = std::move(project);
    manifest_ = std::move(manifest);
    packageDirectory_ = directory.absolutePath();
    playbackFrameCount_ = 0;
    currentFrame_ = 0;
    selectedClipId_.clear();
    hoveredClipId_.clear();
    mediaNames_.clear();
    missingMedia_.clear();

    for (const auto& entry : manifest_.entries()) {
        mediaNames_.emplace(entry.id, entry.name);
        QString mediaPath;
        if (entry.source.kind == heisenberg::MediaSource::Kind::External) {
            mediaPath = QString::fromUtf8(entry.source.path.c_str());
        } else {
            mediaPath = directory.filePath(QString::fromUtf8(entry.source.path.c_str()));
        }
        if (!QFileInfo::exists(mediaPath)) missingMedia_.insert(entry.id);
    }

    if (const auto* active = timeline(); active && project_.viewStates) {
        const auto state = project_.viewStates->find(active->id);
        if (state != project_.viewStates->end()) {
            if (std::isfinite(state->second.zoomScale))
                pixelsPerFrame_ = std::clamp(state->second.zoomScale, kMinZoom, kMaxZoom);
            if (std::isfinite(state->second.scrollOffsetX))
                scrollOffset_ = std::max(0.0, state->second.scrollOffsetX);
            currentFrame_ = std::max<qint64>(0, state->second.playheadFrame);
        }
    }
    clampView();
    update();
    return true;
}

void TimelineWidget::clearProject() {
    project_ = {};
    manifest_ = {};
    mediaNames_.clear();
    missingMedia_.clear();
    packageDirectory_.clear();
    selectedClipId_.clear();
    hoveredClipId_.clear();
    currentFrame_ = 0;
    scrollOffset_ = 0.0;
    update();
}

void TimelineWidget::setFrameCount(qint64 frameCount) {
    playbackFrameCount_ = std::max<qint64>(0, frameCount);
    currentFrame_ = std::clamp<qint64>(currentFrame_, 0, std::max<qint64>(0, contentFrameCount() - 1));
    clampView();
    update();
}

void TimelineWidget::setCurrentFrame(qint64 frame) {
    if (scrubbing_) return;
    currentFrame_ = std::clamp<qint64>(frame, 0, std::max<qint64>(0, contentFrameCount() - 1));
    update();
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

void TimelineWidget::zoomIn() { setZoom(pixelsPerFrame_ * 1.25, width() * 0.6); }
void TimelineWidget::zoomOut() { setZoom(pixelsPerFrame_ / 1.25, width() * 0.6); }

QString TimelineWidget::timelineName() const {
    const auto* active = timeline();
    return active ? QString::fromUtf8(active->name.c_str()) : tr("Timeline");
}

int TimelineWidget::framesPerSecond() const {
    const auto* active = timeline();
    return active && active->fps > 0 ? active->fps : 30;
}

QSize TimelineWidget::sizeHint() const { return QSize(960, 210); }
QSize TimelineWidget::minimumSizeHint() const { return QSize(480, 112); }

const heisenberg::Timeline* TimelineWidget::timeline() const {
    return project_.activeTimeline();
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
    if (active) {
        for (const auto& track : active->tracks) {
            const double trackHeight = boundedTrackHeight(track.displayHeight);
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
            painter.restore();
            y += trackHeight;
        }
    }

    if (!active || active->tracks.empty()) {
        const QRectF headerRect(0, y, kHeaderWidth, kDefaultTrackHeight);
        const QRectF laneRect(kHeaderWidth, y, width() - kHeaderWidth, kDefaultTrackHeight);
        painter.fillRect(headerRect, kSurface);
        painter.fillRect(laneRect, QColor(20, 21, 24));
        painter.setPen(kMutedText);
        painter.drawText(headerRect.adjusted(12, 0, -8, 0), Qt::AlignVCenter | Qt::AlignLeft, tr("VIDEO 1"));
        painter.drawText(laneRect.adjusted(14, 0, -8, 0), Qt::AlignVCenter | Qt::AlignLeft,
                         tr("Drop media here"));
        y += kDefaultTrackHeight;
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

std::string TimelineWidget::clipAt(const QPointF& point) const {
    const auto* active = timeline();
    if (!active || point.x() < kHeaderWidth || point.y() < kRulerHeight) return {};
    double y = kRulerHeight;
    for (const auto& track : active->tracks) {
        const double height = boundedTrackHeight(track.displayHeight);
        if (point.y() >= y && point.y() < y + height) {
            const qint64 frame = frameAtX(point.x());
            for (const auto& clip : track.clips)
                if (clip.contains(frame)) return clip.id;
            return {};
        }
        y += height;
    }
    return {};
}

void TimelineWidget::updateScrub(double x, bool finished) {
    currentFrame_ = frameAtX(x);
    if (finished) emit scrubFinished(currentFrame_);
    else emit scrubFrameRequested(currentFrame_);
    update();
}

void TimelineWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || event->position().x() < kHeaderWidth) {
        QWidget::mousePressEvent(event);
        return;
    }
    selectedClipId_ = clipAt(event->position());
    scrubbing_ = true;
    emit scrubStarted();
    updateScrub(event->position().x(), false);
    event->accept();
}

void TimelineWidget::mouseMoveEvent(QMouseEvent* event) {
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
    setCursor(event->position().x() >= kHeaderWidth ? Qt::PointingHandCursor : Qt::ArrowCursor);
}

void TimelineWidget::mouseReleaseEvent(QMouseEvent* event) {
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
    event->accept();
}

void TimelineWidget::leaveEvent(QEvent* event) {
    hoveredClipId_.clear();
    unsetCursor();
    update();
    QWidget::leaveEvent(event);
}
