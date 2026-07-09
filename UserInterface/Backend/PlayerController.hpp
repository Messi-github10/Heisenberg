//
// Created by NiceFold on 2026/6/30.
//

#pragma once

#include <QObject>
#include <QString>
#include <memory>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

struct AVFrame;

namespace heisenberg {
namespace ctrl   { class PlaybackController; }
namespace render { class VideoPreviewer; }
}

class VideoOutputItem;

namespace heisenberg {
namespace ui {

class PlayerController : public QObject {
    Q_OBJECT

    Q_PROPERTY(bool    isPlaying   READ isPlaying   NOTIFY isPlayingChanged)
    Q_PROPERTY(double  currentTime READ currentTime NOTIFY currentTimeChanged)
    Q_PROPERTY(double  duration    READ duration    NOTIFY durationChanged)
    Q_PROPERTY(bool    isSeekable  READ isSeekable  NOTIFY isSeekableChanged)
    Q_PROPERTY(QString currentFile READ currentFile NOTIFY currentFileChanged)

public:
    explicit PlayerController(QObject* parent = nullptr);
    ~PlayerController() override;

    bool    isPlaying()   const { return isPlaying_; }
    double  currentTime() const { return currentTime_; }
    double  duration()    const { return duration_; }
    bool    isSeekable()  const { return isSeekable_; }
    QString currentFile() const { return currentFile_; }

    void setPlaybackController(heisenberg::ctrl::PlaybackController* ctrl);

public slots:
    void play();
    void pause();
    void togglePlayPause();
    void seek(double seconds);
    void stepForward(int frames = 1);
    void stepBackward(int frames = 1);
    void goToStart();
    void goToEnd();

    Q_INVOKABLE bool openFile(const QString& path);
    Q_INVOKABLE void closeFile();

    /// QML 调用：绑定 VideoOutputItem（子 HWND 就绪后自动初始化管线）
    Q_INVOKABLE void bindVideoOutput(VideoOutputItem* item);

signals:
    void isPlayingChanged();
    void currentTimeChanged();
    void durationChanged();
    void isSeekableChanged();
    void currentFileChanged();

private slots:
    void onFrameDecoded(std::shared_ptr<AVFrame> frame);
    void onNativeWindowReady(HWND hwnd);

private:
    void disconnectPreviewer();

    bool    isPlaying_   = false;
    double  currentTime_ = 0.0;
    double  duration_    = 0.0;
    bool    isSeekable_  = false;
    QString currentFile_;

    heisenberg::ctrl::PlaybackController* ctrl_ = nullptr;
    std::unique_ptr<heisenberg::render::VideoPreviewer> previewer_;

    int videoWidth_  = 0;
    int videoHeight_ = 0;
};

} // namespace ui
} // namespace heisenberg
