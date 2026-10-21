#pragma once

#include <Models/MediaManifest.hpp>
#include <Models/Timeline.hpp>

#include <QWidget>

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace heisenberg::ui { class EditorSession; }
namespace heisenberg { enum class TrimEdge; }

class QMouseEvent;
class QPaintEvent;
class QWheelEvent;
class QColor;
class QMimeData;
class QKeyEvent;
class QContextMenuEvent;

class TimelineWidget final : public QWidget {
    Q_OBJECT

public:
    explicit TimelineWidget(QWidget* parent = nullptr);

    void setSession(heisenberg::ui::EditorSession* session);
    void refreshProject();
    qint64 currentFrame() const {
        return currentFrame_;
    }
    QString selectedClipId() const {
        return QString::fromStdString(selectedClipId_);
    }
    bool cancelInteraction();
    void setSnappingEnabled(bool enabled);
    bool snappingEnabled() const {
        return snappingEnabled_;
    }
    void splitSelectedClip();
    void removeSelectedClip();
    void editSelectedClipAudio();
    void editSelectedClipGrade();
    void editSelectedClipColorGrade();
    void setColorGradePanelEnabled(bool enabled);
    void setFrameCount(qint64 frameCount);
    void setCurrentFrame(qint64 frame);
    void setPlaying(bool playing);
    void fitToContent();
    void zoomIn();
    void zoomOut();

    QString timelineName() const;
    int framesPerSecond() const;

signals:
    void selectedClipChanged(const QString& clipId);
    void colorGradeRequested(const QString& clipId);
    void timelineActivated();
    void scrubStarted();
    void scrubFrameRequested(qint64 frameIndex);
    void scrubFinished(qint64 frameIndex);
    void editFailed(const QString& reason);

protected:
    void contextMenuEvent(QContextMenuEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    bool event(QEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

private:
    void setSelectedClipId(const std::string& clipId);
    const heisenberg::Timeline* timeline() const;
    qint64 contentFrameCount() const;
    qint64 frameAtX(double x) const;
    double xForFrame(qint64 frame) const;
    double maximumScrollOffset() const;
    void clampView();
    void setZoom(double zoom, double anchorX);
    void updateScrub(double x, bool finished);
    std::string clipAt(const QPointF& point) const;
    QString clipName(const heisenberg::Clip& clip) const;
    QColor clipColor(heisenberg::ClipType type, bool missing) const;
    QString formatTimecode(qint64 frame) const;
    QString mediaFromMime(const QMimeData* mime) const;
    qint64 dropFrameAtX(double x) const;
    struct Lane {
        const heisenberg::Track* track = nullptr;
        heisenberg::ClipType type = heisenberg::ClipType::Video;
        double top = 0.0;
        double height = 44.0;
    };
    std::vector<Lane> lanes() const;
    std::optional<Lane> laneAt(const QPointF& point) const;
    std::optional<Lane> laneForTrack(const std::string& trackId) const;
    bool canDropAt(const QPointF& point, const QString& mediaRef = {}) const;
    void updateClipMove(const QPointF& point, Qt::KeyboardModifiers modifiers);
    std::optional<heisenberg::TrimEdge> trimEdgeAt(const heisenberg::Clip& clip, double x) const;

    struct ClipMove {
        std::string clipId;
        std::string trackId;
        QPointF pressPosition;
        QPointF lastPosition;
        double grabOffsetFrames = 0.0;
        std::optional<heisenberg::TrimEdge> trimEdge;
        qint64 originalStartFrame = 0;
        qint64 originalEndFrame = 0;
        qint64 minEdgeFrame = 0;
        qint64 maxEdgeFrame = 0;
        qint64 targetEdgeFrame = 0;
        bool sourceLimited = false;
        qint64 durationFrames = 0;
        qint64 targetFrame = 0;
        qint64 playheadFrame = 0;
        std::optional<qint64> snapFrame;
        Qt::KeyboardModifiers modifiers = Qt::NoModifier;
        bool dragging = false;
        bool validDrop = false;
    };
    std::optional<ClipMove> clipMove_;
    bool snappingEnabled_ = true;
    bool colorGradePanelEnabled_ = false;

    heisenberg::ui::EditorSession* session_ = nullptr;
    std::unordered_map<std::string, std::string> mediaNames_;
    std::unordered_set<std::string> missingMedia_;
    QString draggedMedia_;
    qint64 dropFrame_ = 0;
    std::optional<Lane> dropLane_;
    qint64 playbackFrameCount_ = 0;
    qint64 currentFrame_ = 0;
    double pixelsPerFrame_ = 4.0;
    double scrollOffset_ = 0.0;
    bool playing_ = false;
    bool scrubbing_ = false;
    std::string selectedClipId_;
    std::string hoveredClipId_;
};
