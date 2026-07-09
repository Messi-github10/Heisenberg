//
// Created by NiceFold on 2026/7/7.
//

#pragma once

#include <QObject>
#include <memory>

// 前向声明 FFmpeg C 结构体（完整定义在 .cpp 中）
extern "C" {
struct AVFrame;
}

namespace heisenberg {
namespace ctrl {

/// PlaybackController — 核心播放引擎
///
/// 职责：
///  - 持有 Demuxer + Decoder，管理媒体生命周期
///  - QTimer + QElapsedTimer 驱动 PTS 时钟
///  - 解码循环 → sws_scale YUV→RGB → emit frameReady(QImage)
///  - 不直接接触 QML / Vulkan — 只产出 QImage
///
/// 线程模型：所有操作在主线程（QTimer 驱动）
class PlaybackController : public QObject {
    Q_OBJECT

public:
    enum State {
        Idle,    // 无文件打开
        Playing, // 播放中
        Paused,  // 暂停
        Ended    // 播放到末尾
    };

    explicit PlaybackController(QObject* parent = nullptr);
    ~PlaybackController() override;

    // -- 媒体生命周期 --
    bool open(const std::string& filePath);
    void close();

    // -- 播放控制 --
    void play();
    void pause();
    void togglePlayPause();
    void seek(double seconds);
    void stepForward(int frames = 1);
    void stepBackward(int frames = 1);
    void goToStart();
    void goToEnd();

    // -- 状态查询 --
    State state() const;
    bool isPlaying() const;
    double currentTime() const;   // 秒
    double duration() const;      // 秒
    bool isSeekable() const;
    double fps() const;

signals:
    void stateChanged(heisenberg::ctrl::PlaybackController::State newState);
    void frameDecoded(std::shared_ptr<AVFrame> frame);
    void positionChanged(double seconds);
    void durationChanged(double seconds);
    void endOfStream();

private slots:
    void onTick();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    void setState(State s);
    std::shared_ptr<::AVFrame> decodeFrameForTarget(double targetPtsMs);
    void scheduleNextTick(double targetPtsMs);
    void resetClock(double startPtsMs);
    void decodeFirstFrame();
};

} // namespace ctrl
} // namespace heisenberg
