#pragma once

#include <Models/MediaManifest.hpp>
#include <Models/Timeline.hpp>

#include <QWidget>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>

class QMouseEvent;
class QPaintEvent;
class QWheelEvent;
class QColor;

class TimelineWidget final : public QWidget {
    Q_OBJECT

public:
    explicit TimelineWidget(QWidget* parent = nullptr);

    bool loadPackage(const QString& packageDirectory, QString* error = nullptr);
    void clearProject();
    void setFrameCount(qint64 frameCount);
    void setCurrentFrame(qint64 frame);
    void setPlaying(bool playing);
    void fitToContent();
    void zoomIn();
    void zoomOut();

    QString timelineName() const;
    int framesPerSecond() const;

signals:
    void scrubStarted();
    void scrubFrameRequested(qint64 frameIndex);
    void scrubFinished(qint64 frameIndex);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void leaveEvent(QEvent* event) override;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

private:
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

    heisenberg::ProjectFile project_;
    heisenberg::MediaManifest manifest_;
    std::unordered_map<std::string, std::string> mediaNames_;
    std::unordered_set<std::string> missingMedia_;
    QString packageDirectory_;
    qint64 playbackFrameCount_ = 0;
    qint64 currentFrame_ = 0;
    double pixelsPerFrame_ = 4.0;
    double scrollOffset_ = 0.0;
    bool playing_ = false;
    bool scrubbing_ = false;
    std::string selectedClipId_;
    std::string hoveredClipId_;
};
