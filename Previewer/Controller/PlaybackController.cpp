//
// Created by NiceFold on 2026/7/7.
//

#include "PlaybackController.hpp"

#include <Decoder/AudioDecoder.hpp>
#include <Decoder/DecodeThread.hpp>
#include <Decoder/FFmpegAudioDecoder.hpp>
#include <Renderer/AudioDevice.hpp>
#include <Utiles/Logger.hpp>

extern "C" {
#include <libavutil/frame.h>
}

#include <QMetaObject>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

namespace heisenberg {
namespace ctrl {

using FramePtr = std::shared_ptr<AVFrame>;

struct PlaybackController::Impl {
    RingBuffer<FramePtr> frameBuffer{16};
    DecodeThread         decodeThread{frameBuffer};

    double durationSecs = 0.0;
    double fps          = 0.0;
    bool   seekable     = false;

    PlaybackController::State state = PlaybackController::Idle;
    double  lastDisplayedPtsMs       = -1.0;

    std::shared_ptr<bool> alive{std::make_shared<bool>(true)};

    // ── Consumer thread (MLT-style: dedicated thread + audio-clock master) ──
    std::thread             consumerThread;
    std::atomic<bool>       consumerRunning_{false};
    std::atomic<bool>       playing_{false};
    std::mutex              consumerMutex;
    std::condition_variable consumerCv;

    bool pendingPlayAfterSeek = false;

    // ── Graceful EOF ───────────────────────────────────────
    // Set by onEndOfStream; consumer drains the remaining video frames
    // before transitioning to Ended and waiting for replay.
    std::atomic<bool> decoderEof_{false};

    // Diagnostics
    std::atomic<int64_t> consumerFrames{0};
    std::atomic<int64_t> consumerEmpty{0};

    // ── Audio ────────────────────────────────────────────────
    std::unique_ptr<decoder::IAudioDecoder> audioDecoder;
    std::unique_ptr<renderer::AudioDevice>  audioDevice;
    AudioSpec  audioSpec;
    const float* audioPlanarData  = nullptr;
    int64_t              audioTotalSamples  = 0;
    std::atomic<int64_t> audioSamplePos{0};
    std::atomic<int64_t> audioCallbackCount{0};

    /// Called from the audio I/O thread — must be lock-free and allocation-free.
    void onAudioData(float* output, uint32_t frameCount) {
        const int64_t pos    = audioSamplePos.load(std::memory_order_relaxed);
        const int64_t endPos = pos + frameCount;

        if (!audioPlanarData || pos >= audioTotalSamples || frameCount == 0) {
            std::memset(output, 0,
                        frameCount * audioSpec.channels * sizeof(float));
            return;
        }

        const int64_t actualEnd   = std::min(endPos, audioTotalSamples);
        const int     actualCount = static_cast<int>(actualEnd - pos);
        const int     channels    = audioSpec.channels;

        for (int f = 0; f < actualCount; ++f) {
            for (int ch = 0; ch < channels; ++ch) {
                output[f * channels + ch] =
                    audioPlanarData[ch * audioTotalSamples + pos + f];
            }
        }

        if (actualCount < static_cast<int>(frameCount)) {
            std::memset(output + actualCount * channels, 0,
                        (frameCount - actualCount) * channels * sizeof(float));
        }

        audioSamplePos.store(actualEnd, std::memory_order_relaxed);
    }

    // ── Consumer loop: MLT-style dedicated thread ────────────
    void finishPlayback(PlaybackController* ctrl) {
        {
            std::lock_guard<std::mutex> lock(consumerMutex);
            playing_.store(false, std::memory_order_release);
        }

        double audioEndMs = (audioTotalSamples > 0)
            ? static_cast<double>(audioTotalSamples)
                / static_cast<double>(audioSpec.sampleRate) * 1000.0
            : lastDisplayedPtsMs;
        double finalPosMs = std::max(lastDisplayedPtsMs, audioEndMs);

        LOG_INFO("consumer: reached EOF - video={:.0f}ms audio={:.0f}ms "
                 "final={:.0f}ms containerDuration={:.1f}ms",
                 lastDisplayedPtsMs, audioEndMs, finalPosMs,
                 durationSecs * 1000.0);

        double finalSecs = finalPosMs / 1000.0;
        auto keepAlive = alive;

        QMetaObject::invokeMethod(ctrl, [ctrl, finalSecs, keepAlive] {
            if (!*keepAlive) return;
            if (ctrl->impl_->audioDevice) ctrl->impl_->audioDevice->stop();
            ctrl->impl_->durationSecs = finalSecs;
            emit ctrl->durationChanged(finalSecs);
            emit ctrl->positionChanged(finalSecs);
            ctrl->setState(PlaybackController::Ended);
            emit ctrl->endOfStream();
        }, Qt::QueuedConnection);
    }

    void runConsumer(PlaybackController* ctrl) {
        using Clock     = std::chrono::steady_clock;
        using MilliSec  = std::chrono::duration<double, std::milli>;

        auto       startTime   = Clock::now();
        double     pauseOffset = 0.0;     // ms, used in wall-clock mode

        LOG_INFO("consumer: thread started — audioTotalSamples={} audioSpec={}Hz {}ch",
                 audioTotalSamples, audioSpec.sampleRate, audioSpec.channels);

        while (consumerRunning_.load(std::memory_order_acquire)) {

            // ── Handle paused state ──────────────────────────
            if (!playing_.load(std::memory_order_acquire)) {
                std::unique_lock<std::mutex> lock(consumerMutex);
                consumerCv.wait(lock, [this] {
                    return !consumerRunning_.load(std::memory_order_acquire)
                        || playing_.load(std::memory_order_acquire);
                });
                if (!consumerRunning_.load(std::memory_order_acquire)) break;

                pauseOffset = lastDisplayedPtsMs;
                startTime   = Clock::now();
            }

            // ── Master clock ─────────────────────────────────
            double masterTimeMs;
            bool eof = decoderEof_.load(std::memory_order_acquire);
            if (audioTotalSamples > 0 && !eof) {
                // Audio clock: sample position → milliseconds.
                int64_t pos = audioSamplePos.load(std::memory_order_relaxed);
                masterTimeMs = static_cast<double>(pos)
                             / static_cast<double>(audioSpec.sampleRate) * 1000.0;
            } else {
                // Wall clock (no audio track, or decoder EOF —
                // audio clock may stall before video frames finish).
                auto elapsed = std::chrono::duration_cast<MilliSec>(Clock::now() - startTime);
                masterTimeMs = pauseOffset + elapsed.count();
            }

            // ── Peek next frame ──────────────────────────────
            auto front = frameBuffer.peekFront();
            if (!front.has_value()) {
                if (decoderEof_.load(std::memory_order_acquire)) {
                    LOG_INFO("consumer: buffer empty + decoder EOF - waiting. "
                             "lastVideoPts={:.0f}ms audioTotal={} samples",
                             lastDisplayedPtsMs, audioTotalSamples);
                    finishPlayback(ctrl);
                    continue;
                }
                consumerEmpty.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }

            const double pts   = static_cast<double>((*front)->pts);
            const double delay = pts - masterTimeMs;

            // ── Frame in the future → sleep until just before it's due ──
            if (delay > 2.0) {
                auto sleepUs = static_cast<int64_t>((delay - 2.0) * 1000.0);
                if (sleepUs > 0) {
                    std::unique_lock<std::mutex> lock(consumerMutex);
                    consumerCv.wait_for(lock, std::chrono::microseconds(sleepUs), [this] {
                        return !consumerRunning_.load(std::memory_order_acquire)
                            || !playing_.load(std::memory_order_acquire);
                    });
                }
                continue;
            }

            // ── Frame is due → pop and display ───────────────
            FramePtr frame;
            if (!frameBuffer.popWithTimeout(frame, std::chrono::milliseconds(5))) {
                continue;   // buffer cleared (seek) between peek and pop
            }

            {
                int64_t n = consumerFrames.fetch_add(1, std::memory_order_relaxed) + 1;
                if (n % 60 == 0) {
                    int64_t empty = consumerEmpty.load(std::memory_order_relaxed);
                    LOG_INFO("consumer: frame#{} pts={:.0f}ms master={:.0f}ms delay={:.1f}ms "
                             "emptyWaits={}",
                             n, pts, masterTimeMs, delay, empty);
                }
            }

            if (pts != lastDisplayedPtsMs) {
                lastDisplayedPtsMs = pts;
                auto keepAlive = alive;
                QMetaObject::invokeMethod(ctrl, [ctrl, f = std::move(frame), pts, keepAlive]() mutable {
                    if (!*keepAlive) return;
                    emit ctrl->frameDecoded(std::move(f));
                    emit ctrl->positionChanged(pts / 1000.0);
                }, Qt::QueuedConnection);
            }
        }

        LOG_INFO("consumer: thread stopped");
    }
};

// ============================================================================
//  PlaybackController
// ============================================================================

PlaybackController::PlaybackController(QObject* parent)
    : QObject(parent)
    , impl_(std::make_unique<Impl>()) {}

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

    impl_->decodeThread.onOpened = [this, alive, path = filePath](
        double dur, double fps, bool seekable, FramePtr firstFrame) {
        QMetaObject::invokeMethod(this, [this, alive, path, dur, fps, seekable, firstFrame] {
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

            // ── Open audio decoder ────────────────────────────
            {
                std::unique_ptr<decoder::IAudioDecoder> audioDec;

                auto miniaudioDec = std::make_unique<decoder::AudioDecoder>();
                if (miniaudioDec->open(path)) {
                    audioDec = std::move(miniaudioDec);
                } else {
                    auto ffmpegDec = std::make_unique<decoder::FFmpegAudioDecoder>();
                    if (ffmpegDec->open(path)) {
                        audioDec = std::move(ffmpegDec);
                    }
                }

                if (audioDec && audioDec->isOpen()) {
                    impl_->audioDecoder      = std::move(audioDec);
                    impl_->audioSpec         = impl_->audioDecoder->spec();
                    impl_->audioPlanarData   = impl_->audioDecoder->planarData();
                    impl_->audioTotalSamples = impl_->audioDecoder->totalSamples();
                    impl_->audioSamplePos.store(0, std::memory_order_relaxed);
                    impl_->audioCallbackCount.store(0, std::memory_order_relaxed);
                    LOG_INFO("PlaybackController: audio opened — {} Hz, {} ch, {} samples",
                             impl_->audioSpec.sampleRate,
                             impl_->audioSpec.channels,
                             impl_->audioTotalSamples);
                } else {
                    LOG_INFO("PlaybackController: no audio stream found");
                }
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
        impl_->decoderEof_.store(true, std::memory_order_release);
        LOG_INFO("PlaybackController: decoder EOF");
    };

    impl_->decodeThread.start();
    impl_->decodeThread.open(filePath);
    setState(Loading);
    return true;
}

void PlaybackController::close() {
    *impl_->alive = false;

    // Stop consumer thread
    {
        std::lock_guard<std::mutex> lock(impl_->consumerMutex);
        impl_->consumerRunning_ = false;
        impl_->playing_         = false;
    }
    impl_->consumerCv.notify_all();
    if (impl_->consumerThread.joinable()) {
        impl_->consumerThread.join();
    }
    impl_->consumerFrames.store(0, std::memory_order_relaxed);
    impl_->consumerEmpty.store(0, std::memory_order_relaxed);
    impl_->decoderEof_.store(false, std::memory_order_relaxed);
    impl_->pendingPlayAfterSeek = false;

    impl_->decodeThread.stop();

    // ── Destroy audio ─────────────────────────────────────
    impl_->audioDevice.reset();
    impl_->audioDecoder.reset();
    impl_->audioPlanarData   = nullptr;
    impl_->audioTotalSamples  = 0;
    impl_->audioSamplePos.store(0, std::memory_order_relaxed);
    impl_->audioCallbackCount.store(0, std::memory_order_relaxed);

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
        impl_->pendingPlayAfterSeek = true;
        seek(0.0);
        return;
    }

    // ── Start / resume audio device ──────────────────────
    if (impl_->audioDecoder && impl_->audioDecoder->isOpen()) {
        if (!impl_->audioDevice) {
            impl_->audioDevice = std::make_unique<renderer::AudioDevice>();
            auto callback = [this](float* output, uint32_t frameCount) {
                impl_->onAudioData(output, frameCount);
                ++impl_->audioCallbackCount;
            };
            if (impl_->audioDevice->initialize(impl_->audioSpec, callback)) {
                impl_->audioDevice->start();
                LOG_INFO("PlaybackController: audio device started — {}Hz {}ch",
                         impl_->audioSpec.sampleRate, impl_->audioSpec.channels);
            } else {
                LOG_ERROR("PlaybackController: audio device init failed");
                impl_->audioDevice.reset();
            }
        } else {
            impl_->audioDevice->start();   // resume after pause
        }
    }

    // One consumer thread per open file; EOF only puts it back to sleep.
    bool startConsumer = false;
    {
        std::lock_guard<std::mutex> lock(impl_->consumerMutex);
        impl_->playing_ = true;
        if (!impl_->consumerRunning_.load(std::memory_order_acquire)) {
            impl_->consumerRunning_ = true;
            startConsumer = true;
        }
    }

    if (startConsumer) {
        impl_->consumerThread = std::thread([this] {
            impl_->runConsumer(this);
        });
    }

    impl_->consumerCv.notify_all();
    setState(Playing);

    LOG_INFO("PlaybackController: play — fps={:.2f} audio={}",
             impl_->fps, impl_->audioDevice ? "yes" : "no");
}

void PlaybackController::pause() {
    if (impl_->state != Playing) return;
    {
        std::lock_guard<std::mutex> lock(impl_->consumerMutex);
        impl_->playing_ = false;
    }
    impl_->consumerCv.notify_all();

    if (impl_->audioDevice) impl_->audioDevice->stop();

    setState(Paused);
}

void PlaybackController::togglePlayPause() {
    if (impl_->state == Playing) pause();
    else play();
}

void PlaybackController::seek(double seconds) {
    if (!impl_->seekable) return;

    seconds = std::max(0.0, std::min(seconds, impl_->durationSecs));

    bool wasPlaying = (impl_->state == Playing);

    impl_->decoderEof_.store(false, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(impl_->consumerMutex);
        impl_->playing_ = false;
    }
    impl_->consumerCv.notify_all();
    impl_->frameBuffer.abort();

    if (impl_->audioDevice) impl_->audioDevice->stop();

    if (impl_->audioDecoder && impl_->audioDecoder->isOpen()) {
        impl_->audioSamplePos.store(
            static_cast<int64_t>(seconds) * impl_->audioSpec.sampleRate,
            std::memory_order_relaxed);
    }

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

            const bool shouldResume = wasPlaying || impl_->pendingPlayAfterSeek;
            impl_->pendingPlayAfterSeek = false;

            if (shouldResume) {
                // Wake the persistent consumer after seek completes.
                setState(Paused);
                play();
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

} // namespace ctrl
} // namespace heisenberg
