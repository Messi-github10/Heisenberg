//
// Created by NiceFold on 2026/7/7.
//

#include "PlaybackController.hpp"

#include <Decoder/DecoderFactory.hpp>
#include <Decoder/IDecoder.hpp>
#include <Decoder/SoftwareDecoder.hpp>
#include <Demuxer/DemuxerFactory.hpp>
#include <Demuxer/IDemuxer.hpp>
#include <Common/Stream.hpp>
#include <Common/Packet.hpp>
#include <Common/Codec.hpp>
#include <Utiles/Logger.hpp>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
}

#include <QTimer>
#include <QElapsedTimer>
#include <vector>
#include <cmath>
#include <algorithm>

namespace heisenberg {
namespace ctrl {

// ============================================================
// PIMPL
// ============================================================

struct PlaybackController::Impl {
    // -- 媒体管线 --
    std::unique_ptr<demuxer::IDemuxer> demuxer;
    std::unique_ptr<decoder::IDecoder> decoder;
    const Stream* videoStream = nullptr;

    // -- 媒体元数据 --
    double durationSecs = 0.0;
    double fps = 0.0;

    // -- 播放状态 --
    PlaybackController::State state = PlaybackController::Idle;
    QTimer* timer = nullptr;
    QElapsedTimer wallClock;
    double playbackStartPtsMs = 0.0;   // 本次 play() 起始 PTS（ms）
    double lastDisplayedPtsMs = -1.0;  // 上次显示帧的 PTS（ms）

    // -- 解码缓冲 --
    std::shared_ptr<::AVFrame> pendingFrame;  // 超前解码的下一帧
};

// ============================================================
// 构造 / 析构
// ============================================================

PlaybackController::PlaybackController(QObject* parent)
    : QObject(parent)
    , impl_(std::make_unique<Impl>())
{
    impl_->timer = new QTimer(this);
    impl_->timer->setTimerType(Qt::PreciseTimer);
    connect(impl_->timer, &QTimer::timeout, this, &PlaybackController::onTick);
}

PlaybackController::~PlaybackController() {
    close();
}

// ============================================================
// 状态管理
// ============================================================

PlaybackController::State PlaybackController::state() const { return impl_->state; }
bool PlaybackController::isPlaying() const { return impl_->state == Playing; }
double PlaybackController::currentTime() const { return impl_->lastDisplayedPtsMs / 1000.0; }
double PlaybackController::duration() const { return impl_->durationSecs; }
double PlaybackController::fps() const { return impl_->fps; }

bool PlaybackController::isSeekable() const {
    return impl_->demuxer && impl_->demuxer->seekable();
}

void PlaybackController::setState(State s) {
    if (impl_->state == s) return;
    impl_->state = s;
    emit stateChanged(s);
}

// ============================================================
// 媒体生命周期
// ============================================================

bool PlaybackController::open(const std::string& filePath) {
    close();

    // 1. 创建 Demuxer
    impl_->demuxer = demuxer::createDemuxer();
    if (impl_->demuxer->open(filePath) < 0) {
        LOG_ERROR("PlaybackController: failed to open — {}", filePath);
        impl_->demuxer.reset();
        return false;
    }

    impl_->durationSecs = impl_->demuxer->duration();
    emit durationChanged(impl_->durationSecs);

    // 2. 找视频流
    for (const auto& s : impl_->demuxer->streams()) {
        if (s.isVideo()) {
            impl_->videoStream = &s;
            break;
        }
    }
    if (!impl_->videoStream) {
        LOG_ERROR("PlaybackController: no video stream in {}", filePath);
        close();
        return false;
    }

    impl_->fps = impl_->videoStream->codec.fps();

    // 3. 创建 Decoder
    decoder::DecoderConfig cfg;
    cfg.preferred     = decoder::DecoderBackend::Software;
    cfg.allowFallback = false;

    impl_->decoder = decoder::createDecoder(cfg);
    if (!impl_->decoder || impl_->decoder->open(*impl_->videoStream) < 0) {
        LOG_ERROR("PlaybackController: failed to create decoder");
        close();
        return false;
    }

    // 4. 解码首帧（显示但不播放）
    decodeFirstFrame();

    setState(Paused);
    LOG_INFO("PlaybackController: opened {} — {:.2f}s, {:.2f} fps",
             filePath, impl_->durationSecs, impl_->fps);
    return true;
}

void PlaybackController::close() {
    impl_->timer->stop();
    impl_->pendingFrame.reset();
    impl_->lastDisplayedPtsMs = -1.0;

    if (impl_->decoder) {
        impl_->decoder->close();
        impl_->decoder.reset();
    }
    if (impl_->demuxer) {
        impl_->demuxer->close();
        impl_->demuxer.reset();
    }

    impl_->videoStream = nullptr;
    impl_->fps = 0.0;
    impl_->durationSecs = 0.0;
    setState(Idle);
}

void PlaybackController::decodeFirstFrame() {
    auto frame = decodeFrameForTarget(std::numeric_limits<double>::max());
    if (frame) {
        impl_->lastDisplayedPtsMs = static_cast<double>(frame->pts);
        emit frameDecoded(frame);
        emit positionChanged(frame->pts / 1000.0);
    }
}

// ============================================================
// 播放控制
// ============================================================

void PlaybackController::play() {
    if (impl_->state == Playing) return;
    if (impl_->state == Idle) return;

    if (impl_->state == Ended) {
        seek(0.0);
    }

    double startPtsMs = impl_->lastDisplayedPtsMs;
    if (startPtsMs < 0.0) startPtsMs = 0.0;

    resetClock(startPtsMs);
    setState(Playing);

    // 短间隔启动，onTick 会自己调整
    impl_->timer->start(5);
}

void PlaybackController::pause() {
    if (impl_->state != Playing) return;
    impl_->timer->stop();
    setState(Paused);
}

void PlaybackController::togglePlayPause() {
    if (impl_->state == Playing) {
        pause();
    } else {
        play();
    }
}

void PlaybackController::seek(double seconds) {
    if (!impl_->demuxer || !impl_->demuxer->seekable()) return;

    seconds = std::max(0.0, std::min(seconds, impl_->durationSecs));
    LOG_DEBUG("PlaybackController::seek({:.3f}s)", seconds);

    // 1. 清空解码管线
    impl_->decoder->flush();
    impl_->pendingFrame.reset();

    // 2. Demuxer Seek
    impl_->demuxer->seek(seconds, impl_->videoStream->index, 1 /* AVSEEK_FLAG_BACKWARD */);

    // 3. 定位到目标帧
    double targetPtsMs = seconds * 1000.0;
    auto frame = decodeFrameForTarget(targetPtsMs);
    if (frame) {
        impl_->lastDisplayedPtsMs = static_cast<double>(frame->pts);
        emit frameDecoded(frame);
        emit positionChanged(frame->pts / 1000.0);
    }

    // 4. 如果在播放，重设时钟
    if (impl_->state == Playing) {
        resetClock(impl_->lastDisplayedPtsMs);
        impl_->timer->start(5);
    }
}

void PlaybackController::stepForward(int frames) {
    double step = frames / std::max(impl_->fps, 1.0);
    seek(currentTime() + step);
}

void PlaybackController::stepBackward(int frames) {
    double step = frames / std::max(impl_->fps, 1.0);
    seek(std::max(0.0, currentTime() - step));
}

void PlaybackController::goToStart() { seek(0.0); }
void PlaybackController::goToEnd()   { seek(std::max(0.0, impl_->durationSecs - 0.1)); }

// ============================================================
// 时钟
// ============================================================

void PlaybackController::resetClock(double startPtsMs) {
    impl_->playbackStartPtsMs = startPtsMs;
    impl_->wallClock.start();
}

// ============================================================
// 核心循环 — onTick()
// ============================================================

void PlaybackController::onTick() {
    if (impl_->state != Playing) return;

    // 1. 计算 wall-clock 对应的目标 PTS
    double elapsedMs = static_cast<double>(impl_->wallClock.elapsed());
    double targetPtsMs = impl_->playbackStartPtsMs + elapsedMs;

    // 2. 解码到目标帧
    auto frameToDisplay = decodeFrameForTarget(targetPtsMs);

    // 3. 显示新帧
    if (frameToDisplay) {
        double pts = static_cast<double>(frameToDisplay->pts);
        if (pts != impl_->lastDisplayedPtsMs) {
            impl_->lastDisplayedPtsMs = pts;
            emit frameDecoded(frameToDisplay);
            emit positionChanged(pts / 1000.0);
        }
    }

    // 4. 调整下一 tick 间隔
    scheduleNextTick(targetPtsMs);
}

// ============================================================
// 解码循环 — decodeFrameForTarget()
// ============================================================

std::shared_ptr<::AVFrame> PlaybackController::decodeFrameForTarget(double targetPtsMs) {
    std::shared_ptr<::AVFrame> bestFrame;

    // 先检查 pendingFrame 是否已经过期
    if (impl_->pendingFrame) {
        if (static_cast<double>(impl_->pendingFrame->pts) <= targetPtsMs) {
            bestFrame = std::move(impl_->pendingFrame);
            impl_->pendingFrame.reset();
            // 不 return，继续尝试拿更新的帧
        } else {
            // pendingFrame 还没到显示时间
            return nullptr;
        }
    }

    // 解码循环
    while (true) {
        auto frame = impl_->decoder->receiveFrame();
        if (frame) {
            double framePts = static_cast<double>(frame->pts);
            if (framePts <= targetPtsMs) {
                bestFrame = std::move(frame);
                // 继续拿，找更接近 targetPtsMs 的帧
            } else {
                // 超前了，存到 pendingFrame
                impl_->pendingFrame = std::move(frame);
                break;
            }
        } else {
            // Decoder 需要更多数据
            auto pkt = impl_->demuxer->readPacket();
            if (pkt) {
                if (pkt->streamIndex == impl_->videoStream->index) {
                    impl_->decoder->sendPacket(pkt);
                }
                // else: 跳过非视频包，继续循环
            } else {
                // EOF — 冲洗解码器
                impl_->decoder->sendPacket(nullptr);
                auto drainFrame = impl_->decoder->receiveFrame();
                if (drainFrame) {
                    bestFrame = std::move(drainFrame);
                }
                // 没帧了
                if (!bestFrame && !impl_->pendingFrame) {
                    // 真正结束
                    QMetaObject::invokeMethod(this, [this]() {
                        impl_->timer->stop();
                        setState(Ended);
                        emit endOfStream();
                    }, Qt::QueuedConnection);
                }
                break;
            }
        }
    }

    return bestFrame;
}

// ============================================================
// Tick 间隔调度
// ============================================================

void PlaybackController::scheduleNextTick(double targetPtsMs) {
    if (impl_->fps <= 0.0) {
        impl_->timer->setInterval(16); // 默认 ~60Hz
        return;
    }

    double frameIntervalMs = 1000.0 / impl_->fps;
    double nextBoundaryMs = impl_->lastDisplayedPtsMs + frameIntervalMs;
    double delayMs = nextBoundaryMs - targetPtsMs;

    // 钳制到合理范围
    if (delayMs < 1.0) delayMs = 1.0;
    if (delayMs > frameIntervalMs * 2.0) delayMs = frameIntervalMs;

    impl_->timer->setInterval(static_cast<int>(delayMs));
}
} // namespace ctrl
} // namespace heisenberg
