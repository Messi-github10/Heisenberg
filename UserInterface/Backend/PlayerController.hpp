//
// Created by NiceFold on 2026/6/30.
//

#pragma once

#include <QObject>
#include <QString>

#include <FrameImageProvider.hpp>

// 前向声明 — 避免引入整个 Controller 头文件
namespace heisenberg { namespace ctrl { class PlaybackController; } }

namespace heisenberg {
namespace ui {

class PlayerController : public QObject {
    Q_OBJECT

    // 只读属性 → QML 绑定
    Q_PROPERTY(bool    isPlaying   READ isPlaying   NOTIFY isPlayingChanged)
    Q_PROPERTY(double  currentTime READ currentTime NOTIFY currentTimeChanged)
    Q_PROPERTY(double  duration    READ duration    NOTIFY durationChanged)
    Q_PROPERTY(bool    isSeekable  READ isSeekable  NOTIFY isSeekableChanged)
    Q_PROPERTY(QString currentFile READ currentFile NOTIFY currentFileChanged)

    // 帧版本号（驱动 QML Image 刷新）
    Q_PROPERTY(int frameRevision READ frameRevision NOTIFY frameDecoded)

public:
    explicit PlayerController(QObject* parent = nullptr);
    ~PlayerController() override;

    // 属性 getter
    bool    isPlaying()   const { return isPlaying_; }
    double  currentTime() const { return currentTime_; }
    double  duration()    const { return duration_; }
    bool    isSeekable()  const { return isSeekable_; }
    QString currentFile() const { return currentFile_; }
    int     frameRevision() const { return frameRevision_; }

    // -- 依赖注入 --
    void setPlaybackController(heisenberg::ctrl::PlaybackController* ctrl);
    void setImageProvider(::FrameImageProvider* provider);

public slots:
    // 播放控制 — 全部转发到 PlaybackController
    void play();
    void pause();
    void togglePlayPause();
    void seek(double seconds);
    void stepForward(int frames = 1);
    void stepBackward(int frames = 1);
    void goToStart();
    void goToEnd();

    // 文件加载
    Q_INVOKABLE bool openFile(const QString& path);
    Q_INVOKABLE void closeFile();

signals:
    void isPlayingChanged();
    void currentTimeChanged();
    void durationChanged();
    void isSeekableChanged();
    void currentFileChanged();

    // 通知 QML 有新帧
    void frameDecoded();

private:
    bool    isPlaying_   = false;
    double  currentTime_ = 0.0;
    double  duration_    = 0.0;
    bool    isSeekable_  = false;
    QString currentFile_;
    int     frameRevision_ = 0;

    heisenberg::ctrl::PlaybackController* ctrl_ = nullptr;
    ::FrameImageProvider* imageProvider_ = nullptr;
};

} // namespace ui
} // namespace heisenberg
