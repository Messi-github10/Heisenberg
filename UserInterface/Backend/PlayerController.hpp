//
// Created by NiceFold on 2026/6/30.
//

#pragma once

#include <QObject>
#include <QString>

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

public:
    explicit PlayerController(QObject* parent = nullptr);
    ~PlayerController() override;

    // 属性 getter
    bool    isPlaying()   const { return isPlaying_; }
    double  currentTime() const { return currentTime_; }
    double  duration()    const { return duration_; }
    bool    isSeekable()  const { return isSeekable_; }
    QString currentFile() const { return currentFile_; }

public slots:
    // 播放控制 — QML 信号直接调用
    void play();
    void pause();
    void togglePlayPause();
    void seek(double seconds);
    void stepForward(int frames = 1);
    void stepBackward(int frames = 1);
    void goToStart();
    void goToEnd();

signals:
    void isPlayingChanged();
    void currentTimeChanged();
    void durationChanged();
    void isSeekableChanged();
    void currentFileChanged();

    // 通知 Render 层有新帧需要渲染
    void frameDecoded();

private:
    bool    isPlaying_   = false;
    double  currentTime_ = 0.0;
    double  duration_    = 0.0;
    bool    isSeekable_  = false;
    QString currentFile_;

    // TODO: Phase 2 — 持有 DecoderWrapper + FrameQueue 引用
};

} // namespace ui
} // namespace heisenberg
