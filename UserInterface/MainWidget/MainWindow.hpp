//
// Created by NiceFold on 2026/7/15.
//

#pragma once

#include <QMainWindow>

class VideoWidget;
class TimelineWidget;
class QAction;
class QLabel;
class QCloseEvent;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override = default;

    VideoWidget* videoWidget() const { return videoWidget_; }
    TimelineWidget* timelineWidget() const { return timelineWidget_; }
    bool hardwareDecodeEnabled() const;
    bool loadProjectPackage(const QString& path, bool showError = true);

signals:
    void aboutToClose();
    void playPauseClicked();
    void scrubStarted();
    void scrubFrameRequested(qint64 frameIndex);
    void scrubFinished(qint64 frameIndex);
    void openFileRequested(const QString& path);
    void openPlaylistRequested(const QString& path);
    void openFilterGraphRequested(const QString& path);
    void hardwareDecodeToggled(bool enabled);

public slots:
    void setDuration(double seconds);
    void setFrameCount(qint64 frameCount);
    void setCurrentTime(double seconds);
    void setPlayingState(bool playing);
    void setFilterGraphPath(const QString& path);
    void setFilterGraphError(const QString& message);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void setupUi();
    void setupConnections();

    VideoWidget* videoWidget_ = nullptr;
    TimelineWidget* timelineWidget_ = nullptr;
    QAction* playPauseAction_ = nullptr;
    QAction* hardwareDecodeAction_ = nullptr;
    QLabel* timelineTitleLabel_ = nullptr;
    QLabel* timeLabel_ = nullptr;

    double duration_ = 0.0;
    qint64 frameCount_ = 0;
    bool isPlaying_  = false;
};
