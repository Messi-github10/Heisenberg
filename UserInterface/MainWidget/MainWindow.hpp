//
// Created by NiceFold on 2026/7/15.
//

#pragma once

#include <QMainWindow>

class VideoWidget;
class QSlider;
class QPushButton;
class QLabel;
class QCheckBox;
class QCloseEvent;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override = default;

    VideoWidget* videoWidget() const { return videoWidget_; }
    bool hardwareDecodeEnabled() const;

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
    void exposureChanged(double value);

public slots:
    void setDuration(double seconds);
    void setFrameCount(qint64 frameCount);
    void setCurrentTime(double seconds);
    void setPlayingState(bool playing);
    void setFilterGraphPath(const QString& path);
    void setFilterGraphError(const QString& message);
    void setExposure(double value);
    void setExposureEnabled(bool enabled);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void setupUi();
    void setupConnections();

    // Video area
    VideoWidget* videoWidget_ = nullptr;

    // Controls
    QSlider*     progressBar_ = nullptr;
    QPushButton* playPauseBtn_ = nullptr;
    QPushButton* openFileBtn_  = nullptr;
    QPushButton* openPlaylistBtn_ = nullptr;
    QPushButton* openFilterGraphBtn_ = nullptr;
    QLabel*      timeLabel_    = nullptr;
    QLabel*      filterGraphLabel_ = nullptr;
    QCheckBox*   hardwareDecodeCheck_ = nullptr;
    QSlider*     exposureSlider_ = nullptr;
    QLabel*      exposureLabel_ = nullptr;

    double duration_ = 0.0;
    qint64 frameCount_ = 0;
    bool isPlaying_  = false;
    bool sliderDragging_ = false;
};
