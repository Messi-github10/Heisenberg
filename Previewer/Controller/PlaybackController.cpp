//
// Created by NiceFold on 2026/7/7.
//

#include "PlaybackController.hpp"

#include <Decoder/DecodeThread.hpp>
#include <Utiles/Logger.hpp>

extern "C" {
#include <libavutil/frame.h>
}

#include <QTimer>
#include <QMetaObject>

#include <cmath>
#include <algorithm>

namespace heisenberg {
namespace ctrl {

using FramePtr = std::shared_ptr<AVFrame>;

struct PlaybackController::Impl {
    RingBuffer<FramePtr> frameBuffer{8};
    DecodeThread         decodeThread{frameBuffer};

    double durationSecs = 0.0;
    double fps          = 0.0;
    bool   seekable     = false;

    PlaybackController::State state = PlaybackController::Idle;
    QTimer*      timer              = nullptr;
    QElapsedTimer wallClock;
    double       playbackStartPtsMs  = 0.0;
    double       lastDisplayedPtsMs  = -1.0;

    std::shared_ptr<bool> alive{std::make_shared<bool>(true)};
};

PlaybackController::PlaybackController(QObject* parent)
    : QObject(parent)
    , impl_(std::make_unique<Impl>()) {
    impl_->timer = new QTimer(this);
    impl_->timer->setTimerType(Qt::PreciseTimer);
    connect(impl_->timer, &QTimer::timeout, this, &PlaybackController::onTick);
}

PlaybackController::~PlaybackController() {
    close();
}

PlaybackController::State PlaybackController::state() const {
    return impl_->state;
}

bool PlaybackController::isPlaying() const {
    return impl_->state == Playing;
}

double PlaybackController::currentTime() const {
    return impl_->lastDisplayedPtsMs / 1000.0;
}

double PlaybackController::duration() const {
    return impl_->durationSecs;
}

bool PlaybackController::isSeekable() const {
    return impl_->seekable;
}

double PlaybackController::fps() const {
    return impl_->fps;
}

void PlaybackController::setState(State s) {
    if (impl_->state == s) return;
    impl_->state = s;
    emit stateChanged(s);
}

bool PlaybackController::open(const std::string& filePath) {
    close();

    auto alive = impl_->alive;

    impl_->decodeThread.onOpened = [this, alive](double dur, double fps, bool seekable,
                                                  FramePtr firstFrame) {
        QMetaObject::invokeMethod(this, [this, alive, dur, fps, seekable, firstFrame] {
            if (!*alive) return;
            impl_->durationSecs = dur;
            impl_->fps          = fps;
            impl_->seekable     = seekable;
            emit durationChanged(dur);

            if (firstFrame) {
                impl_->lastDisplayedPtsMs = static_cast<double>(firstFrame->pts);
                emit frameDecoded(firstFrame);
                emit positionChanged(firstFrame->pts / 1000.0);
            }

            setState(Paused);
        }, Qt::QueuedConnection);
    };

    impl_->decodeThread.onOpenFailed = [this, alive](const std::string& reason) {
        QMetaObject::invokeMethod(this, [this, alive, reason] {
            if (!*alive) return;
            LOG_ERROR("PlaybackController: open failed — {}", reason);
            impl_->seekable = false;
            setState(Idle);
        }, Qt::QueuedConnection);
    };

    impl_->decodeThread.onEndOfStream = [this, alive] {
        QMetaObject::invokeMethod(this, [this, alive] {
            if (!*alive) return;
            impl_->timer->stop();
            setState(Ended);
            emit endOfStream();
        }, Qt::QueuedConnection);
    };

    impl_->decodeThread.start();
    impl_->decodeThread.open(filePath);
    setState(Loading);
    return true;
}

void PlaybackController::close() {
    *impl_->alive = false;

    impl_->timer->stop();
    impl_->decodeThread.stop();

    impl_->lastDisplayedPtsMs = -1.0;
    impl_->durationSecs       = 0.0;
    impl_->fps                = 0.0;
    impl_->seekable           = false;

    setState(Idle);

    impl_->alive = std::make_shared<bool>(true);
}

void PlaybackController::play() {
    if (impl_->state == Playing) return;
    if (impl_->state == Idle) return;
    if (impl_->state == Loading) return;

    if (impl_->state == Ended) {
        seek(0.0);
    }

    double startPtsMs = impl_->lastDisplayedPtsMs;
    if (startPtsMs < 0.0) startPtsMs = 0.0;

    resetClock(startPtsMs);
    setState(Playing);
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
    if (!impl_->seekable) return;

    seconds = std::max(0.0, std::min(seconds, impl_->durationSecs));

    impl_->frameBuffer.abort();
    impl_->timer->stop();

    bool wasPlaying = (impl_->state == Playing);

    auto alive = impl_->alive;
    impl_->decodeThread.onOpened = [this, alive, wasPlaying](double dur, double fps,
                                                              bool /*seekable*/,
                                                              FramePtr keyFrame) {
        QMetaObject::invokeMethod(this, [this, alive, dur, fps, wasPlaying, keyFrame] {
            if (!*alive) return;
            impl_->durationSecs = dur;
            impl_->fps          = fps;

            if (keyFrame) {
                impl_->lastDisplayedPtsMs = static_cast<double>(keyFrame->pts);
                emit frameDecoded(keyFrame);
                emit positionChanged(keyFrame->pts / 1000.0);
            }

            if (wasPlaying) {
                resetClock(impl_->lastDisplayedPtsMs);
                setState(Playing);
                impl_->timer->start(5);
            } else {
                setState(Paused);
            }
        }, Qt::QueuedConnection);
    };

    setState(Loading);
    impl_->decodeThread.seek(seconds);
}

void PlaybackController::stepForward(int frames) {
    double step = frames / std::max(impl_->fps, 1.0);
    seek(currentTime() + step);
}

void PlaybackController::stepBackward(int frames) {
    double step = frames / std::max(impl_->fps, 1.0);
    seek(std::max(0.0, currentTime() - step));
}

void PlaybackController::goToStart() {
    seek(0.0);
}

void PlaybackController::goToEnd() {
    seek(std::max(0.0, impl_->durationSecs - 0.1));
}

void PlaybackController::resetClock(double startPtsMs) {
    impl_->playbackStartPtsMs = startPtsMs;
    impl_->wallClock.start();
}

void PlaybackController::onTick() {
    if (impl_->state != Playing) return;

    double elapsedMs   = static_cast<double>(impl_->wallClock.elapsed());
    double targetPtsMs = impl_->playbackStartPtsMs + elapsedMs;

    auto frame = impl_->frameBuffer.popUntil([targetPtsMs](const FramePtr& f) {
        return static_cast<double>(f->pts) > targetPtsMs;
    });

    if (frame.has_value()) {
        double pts = static_cast<double>((*frame)->pts);
        if (pts != impl_->lastDisplayedPtsMs) {
            impl_->lastDisplayedPtsMs = pts;
            FramePtr frameCopy = *frame;
            emit frameDecoded(frameCopy);
            emit positionChanged(pts / 1000.0);
        }
    }

    scheduleNextTick(targetPtsMs);
}

void PlaybackController::scheduleNextTick(double targetPtsMs) {
    if (impl_->fps <= 0.0) {
        impl_->timer->setInterval(16);
        return;
    }

    double frameIntervalMs = 1000.0 / impl_->fps;
    double nextBoundaryMs  = impl_->lastDisplayedPtsMs + frameIntervalMs;
    double delayMs         = nextBoundaryMs - targetPtsMs;

    if (delayMs < 1.0) delayMs = 1.0;
    if (delayMs > frameIntervalMs * 2.0) delayMs = frameIntervalMs;

    impl_->timer->setInterval(static_cast<int>(delayMs));
}

} // namespace ctrl
} // namespace heisenberg
