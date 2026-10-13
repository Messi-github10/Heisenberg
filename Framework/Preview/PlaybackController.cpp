//
// Created by NiceFold on 2026/7/7.
//

#include "PlaybackController.hpp"

#include <MultiMedia/Video/Decoder/DecodeThread.hpp>
#include <Common/AudioFrame.hpp>
#include <Common/FrameTime.hpp>
#include <Common/MediaFrame.hpp>
#include <MultiMedia/Audio/Output/AudioDevice.hpp>
#include <Preview/ProducerPump.hpp>
#include <Preview/TimelineProducer.hpp>
#include <Utiles/Logger.hpp>

extern "C" {
#include <libavutil/frame.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>
#include <utility>

namespace heisenberg {
namespace ctrl {

using FramePtr = std::shared_ptr<AVFrame>;

struct PlaybackController::Impl {
    RingBuffer<MediaFrame> frameBuffer{16};
    RingBuffer<MediaFrame> audioFrameBuffer{64};
    DecodeThread decodeThread{frameBuffer, audioFrameBuffer};
    ProducerPump producerPump{frameBuffer, audioFrameBuffer};
    std::unique_ptr<TimelineProducer> timeline;
    bool usingProducer = false;
    bool hardwareDecode = true;

    double durationSecs = 0.0;
    double fps          = 0.0;
    bool   seekable     = false;

    PlaybackController::State state = PlaybackController::Idle;
    double  lastDisplayedPtsMs       = -1.0;

    PlaybackController::TaskDispatcher dispatcher;
    std::shared_ptr<bool> alive{std::make_shared<bool>(true)};

    // The render thread owns the clock. These only pause and wake it.
    std::atomic<bool>       playing_{false};
    std::mutex              consumerMutex;
    std::condition_variable consumerCv;

    bool pendingPlayAfterSeek = false;
    bool wasPlayingBeforeScrub = false;
    bool scrubEnding = false;
    uint64_t latestScrubRequestId = 0;
    uint64_t endingScrubRequestId = 0;
    int64_t lastRequestedScrubFrame = 0;

    // Frames from an earlier open/seek generation are discarded by consumers.
    std::atomic<uint64_t> activeGeneration{0};

    // Diagnostics
    std::atomic<int64_t> consumerFrames{0};
    std::atomic<int64_t> consumerEmpty{0};

    // ── Audio ────────────────────────────────────────────────
    std::unique_ptr<renderer::AudioDevice>  audioDevice;
    AudioSpec  audioSpec;
    AudioFramePtr currentAudioFrame;
    int currentAudioOffset = 0;
    bool hasAudio = false;
    std::atomic<bool> audioEof{false};
    std::atomic<int64_t> audioSamplePos{0};
    std::atomic<int64_t> audioCallbackCount{0};

    // Called from the audio I/O thread. Queue reads are non-blocking.
    void onAudioData(float* output, uint32_t frameCount) {
        const int channels = audioSpec.channels;
        std::memset(output, 0,
                    static_cast<size_t>(frameCount) * channels * sizeof(float));
        if (!hasAudio || frameCount == 0) return;

        uint32_t written = 0;
        while (written < frameCount) {
            if (!currentAudioFrame) {
                MediaFrame mediaFrame;
                if (!audioFrameBuffer.popWithTimeout(
                        mediaFrame, std::chrono::milliseconds(0))) {
                    break;
                }

                const uint64_t expectedGeneration =
                    activeGeneration.load(std::memory_order_acquire);
                if (mediaFrame.metadata.generation != expectedGeneration)
                    continue;
                if (mediaFrame.type == MediaFrameType::Eof) {
                    audioEof.store(true, std::memory_order_release);
                    break;
                }

                currentAudioFrame = mediaFrame.audioFrame();
                currentAudioOffset = 0;
                if (!currentAudioFrame) continue;
                if (currentAudioFrame->spec().channels != channels ||
                    currentAudioFrame->spec().sampleRate != audioSpec.sampleRate) {
                    currentAudioFrame.reset();
                    continue;
                }
            }

            const int available = currentAudioFrame->samples() -
                                  currentAudioOffset;
            if (available <= 0) {
                currentAudioFrame.reset();
                continue;
            }

            const int count = std::min<int>(
                available, static_cast<int>(frameCount - written));
            for (int sample = 0; sample < count; ++sample) {
                for (int channel = 0; channel < channels; ++channel) {
                    output[(written + sample) * channels + channel] =
                        currentAudioFrame->sampleRo(
                            channel, currentAudioOffset + sample);
                }
            }

            currentAudioOffset += count;
            written += static_cast<uint32_t>(count);
            audioSamplePos.fetch_add(count, std::memory_order_relaxed);
            if (currentAudioOffset >= currentAudioFrame->samples()) {
                currentAudioFrame.reset();
                currentAudioOffset = 0;
            }
        }
    }

    // ── Consumer loop: MLT-style dedicated thread ────────────
    void finishPlayback(PlaybackController* ctrl) {
        {
            std::lock_guard<std::mutex> lock(consumerMutex);
            playing_.store(false, std::memory_order_release);
        }

        double audioEndMs = hasAudio
            ? static_cast<double>(
                  audioSamplePos.load(std::memory_order_relaxed)) /
                static_cast<double>(audioSpec.sampleRate) * 1000.0
            : lastDisplayedPtsMs;
        double finalPosMs = std::max(
            {lastDisplayedPtsMs, audioEndMs, durationSecs * 1000.0});

        LOG_INFO("consumer: reached EOF - video={:.0f}ms audio={:.0f}ms "
                 "final={:.0f}ms containerDuration={:.1f}ms",
                 lastDisplayedPtsMs, audioEndMs, finalPosMs,
                 durationSecs * 1000.0);

        double finalSecs = finalPosMs / 1000.0;
        auto keepAlive = alive;

        ctrl->dispatch([ctrl, finalSecs, keepAlive] {
            if (!*keepAlive) return;
            if (ctrl->impl_->audioDevice) ctrl->impl_->audioDevice->stop();
            ctrl->impl_->durationSecs = finalSecs;
            if (ctrl->onDurationChanged) ctrl->onDurationChanged(finalSecs);
            if (ctrl->onPositionChanged) ctrl->onPositionChanged(finalSecs);
            ctrl->setState(PlaybackController::Ended);
            if (ctrl->onEndOfStream) ctrl->onEndOfStream();
        });
    }

    std::chrono::steady_clock::time_point wallStart = std::chrono::steady_clock::now();
    double wallOffsetMs = 0.0;

    void resetWallClock() {
        wallOffsetMs = std::max(0.0, lastDisplayedPtsMs);
        wallStart = std::chrono::steady_clock::now();
    }

    double masterTimeMs() const {
        if (hasAudio && !audioEof.load(std::memory_order_acquire)) {
            const int64_t pos = audioSamplePos.load(std::memory_order_relaxed);
            return static_cast<double>(pos)
                / static_cast<double>(audioSpec.sampleRate) * 1000.0;
        }
        const auto elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - wallStart);
        return wallOffsetMs + elapsed.count();
    }
};

PlaybackController::Pump PlaybackController::pumpFrame(int& waitMs) {
    waitMs = 2;
    if (!impl_->playing_.load(std::memory_order_acquire)) return Pump::Paused;

    auto front = impl_->frameBuffer.peekFront();
    if (!front.has_value()) {
        impl_->consumerEmpty.fetch_add(1, std::memory_order_relaxed);
        return Pump::Wait;
    }

    const uint64_t expectedGeneration =
        impl_->activeGeneration.load(std::memory_order_acquire);
    if (front->metadata.generation != expectedGeneration) {
        MediaFrame stale;
        impl_->frameBuffer.popWithTimeout(stale, std::chrono::milliseconds(0));
        waitMs = 0;
        return Pump::Presented;
    }

    if (front->isSignal()) {
        if (impl_->hasAudio && !impl_->audioEof.load(std::memory_order_acquire))
            return Pump::Wait;
        MediaFrame eofFrame;
        if (impl_->frameBuffer.popWithTimeout(eofFrame, std::chrono::milliseconds(0))) {
            LOG_INFO("render: received EOF for generation {}", expectedGeneration);
            impl_->finishPlayback(this);
        }
        return Pump::Ended;
    }

    FramePtr frontFrame = front->videoFrame();
    if (!frontFrame) {
        MediaFrame unsupported;
        impl_->frameBuffer.popWithTimeout(unsupported, std::chrono::milliseconds(0));
        waitMs = 0;
        return Pump::Presented;
    }

    const double pts = frameTimeMilliseconds(*frontFrame);
    const double master = impl_->masterTimeMs();
    const double delay = pts - master;
    if (delay > 2.0) {
        waitMs = static_cast<int>(delay);
        return Pump::Wait;
    }

    MediaFrame mediaFrame;
    if (!impl_->frameBuffer.popWithTimeout(mediaFrame, std::chrono::milliseconds(0)))
        return Pump::Wait;
    FramePtr frame = mediaFrame.videoFrame();
    if (!frame) return Pump::Wait;

    const int64_t shown = impl_->consumerFrames.fetch_add(1, std::memory_order_relaxed) + 1;
    if (shown % 60 == 0) {
        LOG_INFO("render: frame#{} pts={:.0f}ms master={:.0f}ms delay={:.1f}ms",
                 shown, pts, master, delay);
    }
    if (pts != impl_->lastDisplayedPtsMs) {
        impl_->lastDisplayedPtsMs = pts;
        if (onFrameDecoded) onFrameDecoded(frame);
        auto keepAlive = impl_->alive;
        dispatch([this, seconds = pts / 1000.0, keepAlive] {
            if (!*keepAlive) return;
            if (onPositionChanged) onPositionChanged(seconds);
        });
    }
    waitMs = 0;
    return Pump::Presented;
}

void PlaybackController::pauseClock() {
    {
        std::lock_guard<std::mutex> lock(impl_->consumerMutex);
        impl_->playing_.store(false, std::memory_order_release);
    }
    impl_->consumerCv.notify_all();
}

// ============================================================================
//  PlaybackController
// ============================================================================

PlaybackController::PlaybackController()
    : impl_(std::make_unique<Impl>()) {}

void PlaybackController::setTaskDispatcher(TaskDispatcher dispatcher) {
    impl_->dispatcher = std::move(dispatcher);
}

void PlaybackController::dispatch(Task task) {
    if (!task) return;
    if (impl_->dispatcher) {
        impl_->dispatcher(std::move(task));
        return;
    }
    task();
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

int64_t PlaybackController::frameCount() const {
    if (impl_->durationSecs <= 0.0 || impl_->fps <= 0.0) return 0;
    return std::max<int64_t>(
        1, static_cast<int64_t>(std::ceil(impl_->durationSecs * impl_->fps)));
}

void PlaybackController::setHardwareDecode(bool enabled) {
    impl_->hardwareDecode = enabled;
    decoder::DecoderConfig config;
    config.preferred = enabled ? decoder::DecoderBackend::D3D11
                               : decoder::DecoderBackend::Software;
    config.allowFallback = enabled;
    impl_->decodeThread.setDecoderConfig(config);
    if (impl_->timeline) impl_->timeline->setHardwareDecode(enabled);
}

const std::vector<TimelineFilter>& PlaybackController::timelineFilters() const {
    static const std::vector<TimelineFilter> empty;
    return empty;
}

void PlaybackController::setState(State s) {
    if (impl_->state == s) return;
    impl_->state = s;
    if (onStateChanged) onStateChanged(s);
}

int64_t PlaybackController::frameFromSeconds(double seconds) const {
    if (impl_->fps <= 0.0) return 0;
    return std::clamp<int64_t>(
        static_cast<int64_t>(std::llround(seconds * impl_->fps)),
        0, std::max<int64_t>(0, frameCount() - 1));
}

bool PlaybackController::open(const std::string& filePath) {
    close();
    impl_->usingProducer = false;

    auto alive = impl_->alive;

    impl_->decodeThread.onOpened = [this, alive](
        double dur, double fps, bool seekable, FramePtr firstFrame,
        uint64_t generation, AudioSpec audioSpec, bool hasAudio) {
        dispatch([this, alive, dur, fps, seekable,
                  firstFrame, generation, audioSpec,
                  hasAudio] {
            if (!*alive) return;
            impl_->activeGeneration.store(generation,
                                          std::memory_order_release);
            impl_->durationSecs = dur;
            impl_->fps          = fps;
            impl_->seekable     = seekable;
            impl_->audioSpec    = audioSpec;
            impl_->hasAudio     = hasAudio && audioSpec.valid();
            impl_->audioEof.store(false, std::memory_order_relaxed);
            impl_->currentAudioFrame.reset();
            impl_->currentAudioOffset = 0;
            impl_->audioSamplePos.store(0, std::memory_order_relaxed);
            impl_->audioCallbackCount.store(0, std::memory_order_relaxed);
            if (onDurationChanged) onDurationChanged(dur);

            if (firstFrame) {
                impl_->lastDisplayedPtsMs = frameTimeMilliseconds(*firstFrame);
                if (onFrameDecoded) onFrameDecoded(firstFrame);
                if (onPositionChanged) onPositionChanged(frameTimeSeconds(*firstFrame));
            }

            LOG_INFO("PlaybackController: streaming audio {}",
                     impl_->hasAudio ? "enabled" : "unavailable");

            setState(Paused);
        });
    };

    impl_->decodeThread.onOpenFailed = [this, alive](const std::string& reason) {
        dispatch([this, alive, reason] {
            if (!*alive) return;
            LOG_ERROR("PlaybackController: open failed — {}", reason);
            impl_->seekable = false;
            setState(Idle);
            if (onOpenFailed) onOpenFailed(reason);
        });
    };

    impl_->decodeThread.onScrubFrame = [this, alive](uint64_t requestId,
                                                      FramePtr frame) {
        dispatch([this, alive, requestId,
                  frame = std::move(frame)]() mutable {
            if (!*alive || impl_->state != Scrubbing) return;
            if (requestId != impl_->latestScrubRequestId) return;

            if (frame) {
                impl_->lastDisplayedPtsMs = frameTimeMilliseconds(*frame);
                const double selectedSeconds = std::max(
                    0.0, impl_->lastDisplayedPtsMs / 1000.0);
                if (impl_->hasAudio) {
                    impl_->audioSamplePos.store(
                        static_cast<int64_t>(selectedSeconds
                                             * impl_->audioSpec.sampleRate),
                        std::memory_order_relaxed);
                }
                if (onFrameDecoded) onFrameDecoded(std::move(frame));
                if (onPositionChanged) onPositionChanged(selectedSeconds);
            }

            if (!impl_->scrubEnding
                || requestId != impl_->endingScrubRequestId) {
                return;
            }

            const bool shouldResume = impl_->wasPlayingBeforeScrub;
            impl_->scrubEnding = false;
            impl_->wasPlayingBeforeScrub = false;
            setState(Paused);
            if (shouldResume) play();
        });
    };

    impl_->decodeThread.start();
    impl_->decodeThread.open(filePath);
    setState(Loading);
    return true;
}

bool PlaybackController::openPlaylist(const std::string& jsonPath) {
    close();
    impl_->usingProducer = true;
    std::string error;
    const auto requested = std::filesystem::u8path(jsonPath);
    const auto packageDir = std::filesystem::is_directory(requested) ? requested : requested.parent_path();
    const auto projectPath = std::filesystem::is_directory(requested) ? requested / "project.json" : requested;
    std::ifstream projectStream(projectPath, std::ios::binary);
    std::string projectText((std::istreambuf_iterator<char>(projectStream)), std::istreambuf_iterator<char>());
    ProjectFile project;
    MediaManifest manifest;
    if (!projectStream) error = "Palmier package is missing project.json";
    if (!projectStream || !ProjectCodec::decode(projectText, project, &error)) {
        LOG_ERROR("PlaybackController: playlist open failed — {}", error);
        impl_->timeline.reset();
        impl_->usingProducer = false;
        setState(Idle);
        if (onOpenFailed) onOpenFailed(error);
        return false;
    }

    std::ifstream mediaStream(packageDir / "media.json", std::ios::binary);
    if (!mediaStream) error = "Palmier package is missing media.json";
    const std::string mediaText((std::istreambuf_iterator<char>(mediaStream)), std::istreambuf_iterator<char>());
    if (!mediaStream || !manifest.loadFromJson(mediaText, &error)) {
        impl_->usingProducer = false;
        setState(Idle);
        if (onOpenFailed) onOpenFailed(error);
        return false;
    }
    const Timeline* active = project.activeTimeline();
    if (!active) {
        impl_->usingProducer = false;
        setState(Idle);
        if (onOpenFailed) onOpenFailed("project has no active timeline");
        return false;
    }
    const auto packageUtf8 = packageDir.u8string();
    return openTimeline(*active, manifest,
        std::string(reinterpret_cast<const char*>(packageUtf8.c_str()), packageUtf8.size()));
}

bool PlaybackController::openTimeline(const Timeline& timeline, const MediaManifest& manifest,
                                      const std::string& projectDir) {
    close();
    impl_->usingProducer = true;
    std::string error;
    impl_->timeline = std::make_unique<TimelineProducer>(
        timeline, manifest, projectDir);
    impl_->timeline->setHardwareDecode(impl_->hardwareDecode);
    if (!impl_->timeline->prepare(&error)) {
        impl_->timeline.reset();
        impl_->usingProducer = false;
        setState(Idle);
        if (onOpenFailed) onOpenFailed(error);
        return false;
    }
    for (const std::string& warning : impl_->timeline->warnings())
        LOG_WARN("Timeline compatibility: {}", warning);

    auto alive = impl_->alive;
    impl_->producerPump.onOpened = [this, alive](
        double dur, double fps, bool seekable, FramePtr firstFrame,
        uint64_t generation, AudioSpec audioSpec, bool hasAudio) {
        dispatch([this, alive, dur, fps, seekable,
                  firstFrame, generation, audioSpec,
                  hasAudio] {
            if (!*alive) return;
            impl_->activeGeneration.store(generation,
                                          std::memory_order_release);
            impl_->durationSecs = dur;
            impl_->fps          = fps;
            impl_->seekable     = seekable;
            impl_->audioSpec    = audioSpec;
            impl_->hasAudio     = hasAudio && audioSpec.valid();
            impl_->audioEof.store(false, std::memory_order_relaxed);
            impl_->currentAudioFrame.reset();
            impl_->currentAudioOffset = 0;
            impl_->audioSamplePos.store(0, std::memory_order_relaxed);
            impl_->audioCallbackCount.store(0, std::memory_order_relaxed);
            if (onDurationChanged) onDurationChanged(dur);

            if (firstFrame) {
                impl_->lastDisplayedPtsMs = frameTimeMilliseconds(*firstFrame);
                if (onFrameDecoded) onFrameDecoded(firstFrame);
                if (onPositionChanged) onPositionChanged(frameTimeSeconds(*firstFrame));
            }
            setState(Paused);
        });
    };
    impl_->producerPump.onOpenFailed = [this, alive](const std::string& reason) {
        dispatch([this, alive, reason] {
            if (!*alive) return;
            LOG_ERROR("PlaybackController: playlist pump failed — {}", reason);
            impl_->seekable = false;
            setState(Idle);
            if (onOpenFailed) onOpenFailed(reason);
        });
    };
    impl_->producerPump.onScrubFrame = [this, alive](uint64_t requestId,
                                                      FramePtr frame) {
        dispatch([this, alive, requestId,
                  frame = std::move(frame)]() mutable {
            if (!*alive || impl_->state != Scrubbing) return;
            if (requestId != impl_->latestScrubRequestId) return;
            if (frame) {
                impl_->lastDisplayedPtsMs = frameTimeMilliseconds(*frame);
                const double selectedSeconds = std::max(
                    0.0, impl_->lastDisplayedPtsMs / 1000.0);
                if (impl_->hasAudio) {
                    impl_->audioSamplePos.store(
                        static_cast<int64_t>(selectedSeconds
                                             * impl_->audioSpec.sampleRate),
                        std::memory_order_relaxed);
                }
                if (onFrameDecoded) onFrameDecoded(std::move(frame));
                if (onPositionChanged) onPositionChanged(selectedSeconds);
            }
            if (!impl_->scrubEnding
                || requestId != impl_->endingScrubRequestId) {
                return;
            }
            const bool shouldResume = impl_->wasPlayingBeforeScrub;
            impl_->scrubEnding = false;
            impl_->wasPlayingBeforeScrub = false;
            setState(Paused);
            if (shouldResume) play();
        });
    };

    impl_->producerPump.start(impl_->timeline.get());
    setState(Loading);
    return true;
}

void PlaybackController::close() {
    *impl_->alive = false;

    {
        std::lock_guard<std::mutex> lock(impl_->consumerMutex);
        impl_->playing_ = false;
    }
    impl_->consumerCv.notify_all();
    impl_->consumerFrames.store(0, std::memory_order_relaxed);
    impl_->consumerEmpty.store(0, std::memory_order_relaxed);
    impl_->activeGeneration.store(0, std::memory_order_relaxed);
    impl_->pendingPlayAfterSeek = false;
    impl_->wasPlayingBeforeScrub = false;
    impl_->scrubEnding = false;
    impl_->latestScrubRequestId = 0;
    impl_->endingScrubRequestId = 0;
    impl_->lastRequestedScrubFrame = 0;

    impl_->decodeThread.stop();
    impl_->producerPump.stop();
    impl_->timeline.reset();
    impl_->usingProducer = false;

    // ── Destroy audio ─────────────────────────────────────
    impl_->audioDevice.reset();
    impl_->currentAudioFrame.reset();
    impl_->currentAudioOffset = 0;
    impl_->hasAudio = false;
    impl_->audioEof.store(false, std::memory_order_relaxed);
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
    if (impl_->state == Scrubbing) return;

    if (impl_->state == Ended) {
        impl_->pendingPlayAfterSeek = true;
        seek(0.0);
        return;
    }

    // ── Start / resume audio device ──────────────────────
    if (impl_->hasAudio) {
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
                impl_->hasAudio = false;
                impl_->audioEof.store(true, std::memory_order_release);
            }
        } else {
            impl_->audioDevice->start();   // resume after pause
        }
    }

    impl_->resetWallClock();
    {
        std::lock_guard<std::mutex> lock(impl_->consumerMutex);
        impl_->playing_.store(true, std::memory_order_release);
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
    if (impl_->state == Scrubbing) return;

    seconds = std::max(0.0, std::min(seconds, impl_->durationSecs));

    bool wasPlaying = (impl_->state == Playing);

    {
        std::lock_guard<std::mutex> lock(impl_->consumerMutex);
        impl_->playing_ = false;
    }
    impl_->consumerCv.notify_all();

    if (impl_->audioDevice) impl_->audioDevice->stop();

    if (impl_->hasAudio) {
        impl_->audioSamplePos.store(
            static_cast<int64_t>(seconds * impl_->audioSpec.sampleRate),
            std::memory_order_relaxed);
    }

    auto alive = impl_->alive;
    auto completeSeek = [this, alive, wasPlaying](double dur, double fps,
                                                              bool /*seekable*/,
                                                              FramePtr keyFrame,
                                                              uint64_t generation,
                                                              AudioSpec audioSpec,
                                                              bool hasAudio) {
        dispatch([this, alive, dur, fps, wasPlaying,
                  keyFrame, generation, audioSpec,
                  hasAudio] {
            if (!*alive) return;
            impl_->activeGeneration.store(generation,
                                          std::memory_order_release);
            impl_->durationSecs = dur;
            impl_->fps          = fps;
            impl_->audioSpec    = audioSpec;
            impl_->hasAudio     = hasAudio && audioSpec.valid();
            impl_->audioEof.store(false, std::memory_order_relaxed);
            impl_->currentAudioFrame.reset();
            impl_->currentAudioOffset = 0;

            if (keyFrame) {
                impl_->lastDisplayedPtsMs = frameTimeMilliseconds(*keyFrame);
                if (impl_->hasAudio) {
                    const double selectedSeconds = std::max(
                        0.0, impl_->lastDisplayedPtsMs / 1000.0);
                    impl_->audioSamplePos.store(
                        static_cast<int64_t>(selectedSeconds * impl_->audioSpec.sampleRate),
                        std::memory_order_relaxed);
                }
                if (onFrameDecoded) onFrameDecoded(keyFrame);
                if (onPositionChanged) onPositionChanged(frameTimeSeconds(*keyFrame));
            }

            const bool shouldResume = wasPlaying || impl_->pendingPlayAfterSeek;
            impl_->pendingPlayAfterSeek = false;

            if (shouldResume) {
                setState(Paused);
                play();
            } else {
                setState(Paused);
            }
        });
    };

    if (impl_->usingProducer) impl_->producerPump.onOpened = std::move(completeSeek);
    else impl_->decodeThread.onOpened = std::move(completeSeek);

    setState(Loading);
    if (impl_->usingProducer) {
        impl_->producerPump.seek(frameFromSeconds(seconds),
                                 frameFromSeconds(currentTime()));
    } else {
        impl_->decodeThread.seek(seconds, std::max(0.0, currentTime()));
    }
}

void PlaybackController::beginScrub() {
    if (!impl_->seekable) return;
    if (impl_->state == Idle || impl_->state == Loading
        || impl_->state == Scrubbing) {
        return;
    }

    impl_->wasPlayingBeforeScrub = (impl_->state == Playing);
    impl_->scrubEnding = false;
    impl_->endingScrubRequestId = 0;
    impl_->lastRequestedScrubFrame = std::clamp<int64_t>(
        static_cast<int64_t>(std::llround(
            std::max(0.0, currentTime()) * std::max(impl_->fps, 1.0))),
        0, std::max<int64_t>(0, frameCount() - 1));

    {
        std::lock_guard<std::mutex> lock(impl_->consumerMutex);
        impl_->playing_ = false;
    }
    impl_->consumerCv.notify_all();
    if (impl_->audioDevice) impl_->audioDevice->stop();

    if (impl_->usingProducer) impl_->producerPump.beginScrub();
    else impl_->decodeThread.beginScrub();
    setState(Scrubbing);
}

void PlaybackController::scrubToFrame(int64_t frameIndex) {
    if (impl_->state != Scrubbing || impl_->scrubEnding) return;

    frameIndex = std::clamp<int64_t>(
        frameIndex, 0, std::max<int64_t>(0, frameCount() - 1));
    const int64_t originFrame = impl_->lastRequestedScrubFrame;
    impl_->lastRequestedScrubFrame = frameIndex;
    const uint64_t requestId = ++impl_->latestScrubRequestId;
    if (impl_->usingProducer) {
        impl_->producerPump.scrubToFrame(frameIndex, originFrame,
                                         requestId, false);
    } else {
        impl_->decodeThread.scrubToFrame(frameIndex, originFrame,
                                         requestId, false);
    }
}

void PlaybackController::endScrub(int64_t frameIndex) {
    if (impl_->state != Scrubbing || impl_->scrubEnding) return;

    frameIndex = std::clamp<int64_t>(
        frameIndex, 0, std::max<int64_t>(0, frameCount() - 1));
    const int64_t originFrame = impl_->lastRequestedScrubFrame;
    impl_->lastRequestedScrubFrame = frameIndex;
    impl_->scrubEnding = true;
    impl_->endingScrubRequestId = ++impl_->latestScrubRequestId;
    if (impl_->usingProducer) {
        impl_->producerPump.scrubToFrame(frameIndex, originFrame,
                                         impl_->endingScrubRequestId, true);
    } else {
        impl_->decodeThread.scrubToFrame(frameIndex, originFrame,
                                         impl_->endingScrubRequestId, true);
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

void PlaybackController::goToStart() {
    seek(0.0);
}

void PlaybackController::goToEnd() {
    seek(std::max(0.0, impl_->durationSecs - 0.1));
}

} // namespace ctrl
} // namespace heisenberg
