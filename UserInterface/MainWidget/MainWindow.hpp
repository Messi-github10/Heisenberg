//
// Created by NiceFold on 2026/7/15.
//

#pragma once

#include <QMainWindow>

class VideoWidget;
class QSlider;
class QPushButton;
class QLabel;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override = default;

    VideoWidget* videoWidget() const { return videoWidget_; }

signals:
    void playPauseClicked();
    void seekRequested(double seconds);
    void openFileRequested(const QString& path);

public slots:
    void setDuration(double seconds);
    void setCurrentTime(double seconds);
    void setPlayingState(bool playing);

private:
    void setupUi();
    void setupConnections();

    // Video area
    VideoWidget* videoWidget_ = nullptr;

    // Controls
    QSlider*     progressBar_ = nullptr;
    QPushButton* playPauseBtn_ = nullptr;
    QPushButton* openFileBtn_  = nullptr;
    QLabel*      timeLabel_    = nullptr;

    double duration_ = 0.0;
    bool isPlaying_  = false;
    bool sliderDragging_ = false;
};
