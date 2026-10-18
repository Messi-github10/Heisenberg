#include "VideoEngine.hpp"
#include <Compositing/CompositionBuilder.hpp>
#include <Models/ProjectCodec.hpp>
#include <Models/ProjectFile.hpp>
#include <MultiMedia/Audio/Output/AudioDevice.hpp>
#include <Utiles/Logger.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>

namespace heisenberg {
namespace {
using Clock = std::chrono::steady_clock;
constexpr size_t kVideoCapacity = 12;
constexpr size_t kAudioCapacity = 48;
}

struct VideoEngine::Impl {
    explicit Impl(VideoEngine* value) : owner(value) { worker = std::thread([this] { run(); }); }
    VideoEngine* owner;
    mutable std::mutex mutex;
    std::condition_variable cv;
    Snapshot state;
    TaskDispatcher dispatcher;
    struct Lifetime { std::atomic<bool> alive{true}; std::atomic<uint64_t> generation{0}; };
    std::shared_ptr<Lifetime> lifetime = std::make_shared<Lifetime>();
    std::atomic<uint64_t> active{0};
    bool stopping = false, hardware = true, resumeAfterScrub = false;
    struct Request {
        enum Kind { Close, Source, Timeline, Frame } kind = Close;
        uint64_t generation = 0;
        std::string path;
        CompositionPlan plan;
        int64_t frame = 0;
        bool scrub = false, resume = false, hardware = true;
    };
    std::optional<Request> request;
    struct Queued { DecodedFrame frame; bool urgent = false, eof = false; };
    std::deque<Queued> videoQueue;
    struct AudioItem { std::shared_ptr<AudioFrame> frame; uint64_t generation = 0; };
    std::deque<AudioItem> audioQueue;
    std::shared_ptr<AudioFrame> currentAudio;
    int audioOffset = 0;
    uint64_t audioGeneration = 0;
    AudioSpec audioSpec;
    std::atomic<int64_t> audioPosition{0};
    std::mutex deviceMutex;
    std::unique_ptr<renderer::AudioDevice> device;
    bool audioClock = false, audioOutputEnabled = true, discardAudio = false;
    bool manualClock = false;
    double manualNow = 0.0, manualStart = 0.0;
    Clock::time_point wallStart = Clock::now();
    double wallOffset = 0.0;
    std::thread worker;
    // The fields below are touched by the decode worker only.
    CompositionPlan plan;
    bool sourceMode = false;
    CompositionExecutor executor;
    int64_t nextFrame = 0;
    bool ready = false, eofQueued = false;
    struct DecodeDiagnostics {
        uint64_t generation = 0, frames = 0, commands = 0;
        double videoMs = 0.0, audioMs = 0.0, maxTotalMs = 0.0;
        int64_t worstFrame = 0;
        std::string clipId;
        Clock::time_point last{};
    } decodeDiagnostics;
    uint64_t diagnosticUnderruns = 0, diagnosticMissingSamples = 0, diagnosticEmptyVideoPolls = 0;
    uint64_t diagnosticPresented = 0, diagnosticLateFrames = 0;
    double diagnosticMaxLateMs = 0.0;
    double clockSecondsLocked() const {
        return wallOffset + (manualClock ? manualNow - manualStart
            : std::chrono::duration<double>(Clock::now() - wallStart).count());
    }
    void anchorClockLocked(double seconds) {
        wallOffset = seconds; wallStart = Clock::now(); manualStart = manualNow;
    }

    template<class Callback, class... Args>
    void notify(uint64_t generation, Callback callback, Args... args) {
        if (!callback) return;
        TaskDispatcher dispatch;
        { std::lock_guard lock(mutex); dispatch = dispatcher; }
        auto token = lifetime;
        auto task = [token, generation, callback = std::move(callback), args...] {
            if (!token->alive.load(std::memory_order_acquire)
                || token->generation.load(std::memory_order_acquire) != generation) return;
            callback(args...);
        };
        if (dispatch) dispatch(std::move(task)); else task();
    }
    void wake() { if (owner->onWakeRender) owner->onWakeRender(); }
    void stopAudio() {
        std::lock_guard deviceLock(deviceMutex);
        if (device) device->stop();
        std::lock_guard lock(mutex);
        audioClock = false;
    }
    void audioData(float* output, uint32_t samples) {
        std::lock_guard lock(mutex);
        const int channels = audioSpec.channels;
        std::memset(output, 0, static_cast<size_t>(samples) * channels * sizeof(float));
        if (state.state != State::Playing) return;
        uint32_t written = 0;
        while (written < samples) {
            if (audioGeneration != state.generation) { currentAudio.reset(); audioOffset = 0; }
            if (!currentAudio) {
                while (!audioQueue.empty() && audioQueue.front().generation != state.generation) audioQueue.pop_front();
                if (audioQueue.empty()) break;
                audioGeneration = audioQueue.front().generation;
                currentAudio = std::move(audioQueue.front().frame);
                audioQueue.pop_front(); audioOffset = 0;
            }
            const int count = std::min<int>(currentAudio->samples() - audioOffset, samples - written);
            if (count <= 0) { currentAudio.reset(); continue; }
            for (int s = 0; s < count; ++s)
                for (int c = 0; c < channels; ++c)
                    output[(written + s) * channels + c] = currentAudio->sampleRo(c, audioOffset + s);
            audioPosition.store(currentAudio->position() + audioOffset + count, std::memory_order_relaxed);
            written += count; audioOffset += count;
            if (audioOffset >= currentAudio->samples()) currentAudio.reset();
        }
        if (written < samples) {
            ++diagnosticUnderruns;
            diagnosticMissingSamples += samples - written;
        }
        cv.notify_all();
    }
    void startAudio() {
        std::lock_guard deviceLock(deviceMutex);
        AudioSpec spec;
        { std::lock_guard lock(mutex);
          if (state.state != State::Playing || stopping || !audioOutputEnabled) return;
          spec = audioSpec; }
        if (!spec.valid()) return;
        if (!device) {
            auto candidate = std::make_unique<renderer::AudioDevice>();
            if (!candidate->initialize(spec, [this](float* out, uint32_t n) { audioData(out, n); })) {
                std::lock_guard lock(mutex);
                discardAudio = true; audioQueue.clear(); currentAudio.reset(); audioOffset = 0;
                cv.notify_all(); return;
            }
            device = std::move(candidate);
        }
        const bool started = device->start();
        std::lock_guard lock(mutex);
        audioClock = started;
        discardAudio = !started;
        if (!started) { audioQueue.clear(); currentAudio.reset(); audioOffset = 0; }
        cv.notify_all();
    }
    uint64_t invalidateLocked() {
        const auto generation = active.fetch_add(1, std::memory_order_acq_rel) + 1;
        lifetime->generation.store(generation, std::memory_order_release);
        state.generation = generation;
        diagnosticUnderruns = diagnosticMissingSamples = diagnosticEmptyVideoPolls = 0;
        diagnosticPresented = diagnosticLateFrames = 0;
        diagnosticMaxLateMs = 0.0;
        videoQueue.clear(); audioQueue.clear(); currentAudio.reset(); audioOffset = 0;
        return generation;
    }
    void fail(uint64_t generation, const std::string& message) {
        { std::lock_guard lock(mutex);
          if (state.generation != generation) return;
          state.state = State::Idle; state.seekable = false; }
        notify(generation, owner->onStateChanged, State::Idle);
        notify(generation, owner->onOpenFailed, message);
    }
    CompositionExecutor::Context context(uint64_t generation) {
        return {generation, [this, generation] {
            return active.load(std::memory_order_acquire) != generation;
        }};
    }
    void reportDecode(int64_t frame, uint64_t generation, const DecodedFrame& video,
                      double videoMs, double audioMs) {
        auto& stats = decodeDiagnostics;
        if (stats.generation != generation || stats.clipId != video.clipId) {
            const auto commands = stats.commands;
            stats = {}; stats.generation = generation; stats.clipId = video.clipId; stats.commands = commands;
        }
        ++stats.frames; stats.videoMs += videoMs; stats.audioMs += audioMs;
        if (videoMs + audioMs > stats.maxTotalMs) {
            stats.maxTotalMs = videoMs + audioMs; stats.worstFrame = frame;
        }
        const auto now = Clock::now();
        if (stats.last != Clock::time_point{} && now - stats.last < std::chrono::seconds(1)) return;
        size_t videoQueued, audioQueued;
        uint64_t underruns, missingSamples, emptyPolls, presented, late;
        int64_t audioSample;
        double maxLate, currentTime;
        bool usingAudioClock, discardingAudio;
        {
            std::lock_guard lock(mutex);
            videoQueued = videoQueue.size(); audioQueued = audioQueue.size();
            underruns = diagnosticUnderruns; missingSamples = diagnosticMissingSamples;
            emptyPolls = diagnosticEmptyVideoPolls; presented = diagnosticPresented;
            late = diagnosticLateFrames; maxLate = diagnosticMaxLateMs;
            audioSample = audioPosition.load(std::memory_order_relaxed);
            currentTime = state.currentTime; usingAudioClock = audioClock; discardingAudio = discardAudio;
            diagnosticUnderruns = diagnosticMissingSamples = diagnosticEmptyVideoPolls = 0;
            diagnosticPresented = diagnosticLateFrames = 0; diagnosticMaxLateMs = 0.0;
        }
        LOG_INFO("[PlaybackDiag] engine gen={} clip='{}' media='{}' source_mode={} target_frame={} "
                 "target_s={:.3f} current_s={:.3f} frames={} commands={} video_avg_ms={:.3f} audio_avg_ms={:.3f} "
                 "decode_max_ms={:.3f} worst_frame={} budget_ms={:.3f} video_queue={} audio_queue={} "
                 "presented={} late_frames={} max_late_ms={:.3f} empty_video_polls={} "
                 "audio_underruns={} missing_audio_samples={} audio_position={} audio_clock={} discard_audio={}",
                 generation, video.clipId, video.mediaRef, sourceMode, frame, plan.settings.secondsFromFrame(frame),
                 currentTime, stats.frames, stats.commands, stats.videoMs / stats.frames, stats.audioMs / stats.frames,
                 stats.maxTotalMs, stats.worstFrame, 1000.0 / plan.settings.fps(), videoQueued, audioQueued,
                 presented, late, maxLate, emptyPolls, underruns, missingSamples, audioSample,
                 usingAudioClock, discardingAudio);
        Logger::ModuleLogger(__FILE__)->flush();
        stats.last = now; stats.frames = stats.commands = 0;
        stats.videoMs = stats.audioMs = stats.maxTotalMs = 0.0;
    }
    bool execute(int64_t frame, uint64_t generation, bool urgent) {
        const auto diagnosticStart = Clock::now();
        auto video = executor.videoFrame(frame, context(generation));
        if (video.status == CompositionStatus::Cancelled) return false;
        if (!video) { ready = false; fail(generation, video.error); return false; }
        const auto diagnosticVideoEnd = Clock::now();
        auto audio = executor.audioSamples(plan.settings.audioSamplePosition(frame),
            plan.settings.audioSamplePosition(frame + 1), context(generation));
        if (audio.status == CompositionStatus::Cancelled) return false;
        if (audio.status == CompositionStatus::Failure) {
            ready = false; fail(generation, audio.error); return false;
        }
        const auto diagnosticAudioEnd = Clock::now();
        reportDecode(frame, generation, video.value,
            std::chrono::duration<double, std::milli>(diagnosticVideoEnd - diagnosticStart).count(),
            std::chrono::duration<double, std::milli>(diagnosticAudioEnd - diagnosticVideoEnd).count());
        const auto sound = audio.value;
        // The observer still receives exactly the PCM sent to the audio queue,
        // including silence, on the existing sole decode worker.
        if (sound && owner->onAudioPrepared && active.load(std::memory_order_acquire) == generation)
            owner->onAudioPrepared(sound, generation);
        { std::lock_guard lock(mutex);
          if (state.generation != generation) return false;
          state.warnings = executor.warnings();
          videoQueue.push_back({std::move(video.value), urgent, false});
          if (sound && audioOutputEnabled && !discardAudio) audioQueue.push_back({sound, generation}); }
        wake();
        return true;
    }
    void run() {
        bool decodeHardware = true;
        uint64_t generation = 0;
        for (;;) {
            std::optional<Request> command;
            { std::unique_lock lock(mutex);
              cv.wait(lock, [&] { return stopping || request || (ready && active.load(std::memory_order_acquire) == generation && !eofQueued
                  && state.state != State::Scrubbing && videoQueue.size() < kVideoCapacity
                  && audioQueue.size() < kAudioCapacity); });
              if (stopping) break;
              if (request) { command = std::move(request); request.reset(); } }
            if (command) {
                ++decodeDiagnostics.commands;
                generation = command->generation;
                decodeHardware = command->hardware;
                if (command->kind == Request::Close) { executor.close(); ready = false; continue; }
                if (command->kind == Request::Source || command->kind == Request::Timeline) {
                    ready = false;
                    CompositionExecutor::Options options;
                    options.hardwareDecode = decodeHardware;
                    options.errorPolicy = CompositionErrorPolicy::Preview;
                    auto opened = command->kind == Request::Source
                        ? executor.openSource(command->path, options, context(generation))
                        : executor.open(command->plan, options, context(generation));
                    if (opened.status == CompositionStatus::Cancelled) continue;
                    if (!opened) { fail(generation, opened.error); continue; }
                    plan = executor.plan();
                    sourceMode = opened.value.sourceMode;
                    const auto& info = opened.value;
                    { std::lock_guard lock(mutex);
                      if (state.generation != generation) continue;
                      state.duration = info.duration;
                      state.fps = plan.settings.fps(); state.frameCount = info.frameCount;
                      state.seekable = info.seekable;
                      state.hasVideo = info.hasVideo;
                      state.audioOnly = !info.hasVideo && info.hasAudio;
                      state.warnings = executor.warnings();
                      audioSpec = plan.settings.audioSpec(); }
                    notify(generation, owner->onDurationChanged, info.duration);
                }
                // Reset even nearby seek/scrub requests: generation changes must
                // not retain decoder delay, cached images or resampler history.
                if (command->kind == Request::Frame) {
                    CompositionExecutor::Options options;
                    options.hardwareDecode = decodeHardware;
                    options.errorPolicy = CompositionErrorPolicy::Preview;
                    auto opened = sourceMode
                        ? executor.openSource(plan.clips.front().sourcePath, options, context(generation))
                        : executor.open(plan, options, context(generation));
                    if (opened.status == CompositionStatus::Cancelled) continue;
                    if (!opened) { ready = false; fail(generation, opened.error); continue; }
                }
                nextFrame = command->frame; eofQueued = false;
                if (plan.frameCount <= 0) {
                    { std::lock_guard lock(mutex); if (state.generation == generation) state.state = State::Paused; }
                    notify(generation, owner->onStateChanged, State::Paused);
                    continue;
                }
                if (!execute(nextFrame, generation, true)) continue;
                ++nextFrame; ready = true;
                const auto nextState = command->scrub ? State::Scrubbing : State::Paused;
                { std::lock_guard lock(mutex);
                  if (state.generation != generation) continue;
                  state.state = nextState;
                  state.currentTime = plan.settings.secondsFromFrame(command->frame);
                  anchorClockLocked(state.currentTime);
                  audioPosition.store(plan.settings.audioSamplePosition(command->frame), std::memory_order_relaxed); }
                notify(generation, owner->onStateChanged, nextState);
                if (command->resume && !command->scrub) owner->play();
                wake();
                continue;
            }
            if (nextFrame >= plan.frameCount) {
                std::lock_guard lock(mutex);
                if (state.generation == generation) {
                    DecodedFrame end; end.generation = generation;
                    videoQueue.push_back({end, false, true}); eofQueued = true;
                }
                continue;
            }
            if (execute(nextFrame, generation, false)) ++nextFrame;
        }
        executor.close();
    }
};

VideoEngine::VideoEngine() : impl_(std::make_unique<Impl>(this)) {}
VideoEngine::~VideoEngine() { shutdown(); }
void VideoEngine::setTaskDispatcher(TaskDispatcher dispatcher) {
    std::lock_guard lock(impl_->mutex); impl_->dispatcher = std::move(dispatcher);
}
VideoEngine::Snapshot VideoEngine::snapshot() const { std::lock_guard lock(impl_->mutex); return impl_->state; }
bool VideoEngine::isCurrentGeneration(uint64_t value) const { return impl_->active.load(std::memory_order_acquire) == value; }
void VideoEngine::setHardwareDecode(bool enabled) { std::lock_guard lock(impl_->mutex); impl_->hardware = enabled; }
void VideoEngine::setAudioOutputEnabled(bool enabled) {
    impl_->stopAudio();
    { std::lock_guard lock(impl_->mutex);
      impl_->audioOutputEnabled = enabled; impl_->discardAudio = !enabled;
      impl_->audioQueue.clear(); impl_->currentAudio.reset(); impl_->audioOffset = 0;
      impl_->anchorClockLocked(impl_->state.currentTime); }
    impl_->cv.notify_all();
    if (enabled) impl_->startAudio();
}
void VideoEngine::setManualClock(double seconds) {
    if (!std::isfinite(seconds)) return;
    { std::lock_guard lock(impl_->mutex);
      if (!impl_->manualClock) { impl_->manualStart = seconds; impl_->manualClock = true; }
      impl_->manualNow = seconds; }
    impl_->cv.notify_all(); impl_->wake();
}

bool VideoEngine::open(const std::string& path) {
    impl_->stopAudio();
    uint64_t generation;
    { std::lock_guard lock(impl_->mutex);
      if (impl_->stopping) return false;
      generation = impl_->invalidateLocked(); impl_->state = Snapshot{};
      impl_->state.generation = generation; impl_->state.state = State::Loading;
      Impl::Request command; command.kind = Impl::Request::Source; command.path = path;
      command.generation = generation; command.hardware = impl_->hardware;
      impl_->request = std::move(command); }
    impl_->notify(generation, onStateChanged, State::Loading); impl_->cv.notify_all(); return true;
}
bool VideoEngine::openTimeline(const Timeline& timeline, const MediaManifest& manifest, const std::string& dir) {
    CompositionPlan plan;
    std::string error;
    if (!CompositionBuilder::build(timeline, manifest, dir, plan, &error)) {
        close(); impl_->notify(snapshot().generation, onOpenFailed, error); return false;
    }
    return openComposition(plan);
}
bool VideoEngine::openTimeline(const Timeline& timeline, const MediaManifest& manifest,
                               const std::string& dir, const ColorSettings& colorSettings) {
    CompositionPlan plan;
    std::string error;
    if (!CompositionBuilder::build(timeline, manifest, dir, colorSettings, plan, &error)) {
        close(); impl_->notify(snapshot().generation, onOpenFailed, error); return false;
    }
    return openComposition(plan);
}
bool VideoEngine::openSnapshot(const ProjectSnapshot& snapshotValue) {
    CompositionPlan plan;
    std::string error;
    if (!CompositionBuilder::build(snapshotValue, plan, &error)) {
        close(); impl_->notify(snapshot().generation, onOpenFailed, error); return false;
    }
    return openComposition(plan);
}
bool VideoEngine::openComposition(const CompositionPlan& plan) {
    for (const auto& warning : plan.warnings) LOG_WARN("Composition: {}", warning);
    impl_->stopAudio(); uint64_t generation;
    { std::lock_guard lock(impl_->mutex);
      if (impl_->stopping) return false;
      generation = impl_->invalidateLocked(); impl_->state = Snapshot{};
      impl_->state.generation = generation; impl_->state.state = State::Loading;
      Impl::Request command; command.kind = Impl::Request::Timeline; command.plan = plan;
      command.generation = generation; command.hardware = impl_->hardware;
      impl_->state.warnings = command.plan.warnings;
      impl_->request = std::move(command); }
    impl_->notify(generation, onStateChanged, State::Loading); impl_->cv.notify_all(); return true;
}
bool VideoEngine::openPlaylist(const std::string& path) {
    const auto requested = std::filesystem::u8path(path);
    const auto directory = std::filesystem::is_directory(requested) ? requested : requested.parent_path();
    const auto projectPath = std::filesystem::is_directory(requested) ? requested / "project.json" : requested;
    std::ifstream projectStream(projectPath, std::ios::binary), mediaStream(directory / "media.json", std::ios::binary);
    std::string projectText((std::istreambuf_iterator<char>(projectStream)), {});
    std::string mediaText((std::istreambuf_iterator<char>(mediaStream)), {});
    ProjectFile project; MediaManifest manifest; std::string error;
    if (!projectStream || !mediaStream) error = "Palmier package requires project.json and media.json";
    else if (!ProjectCodec::decode(projectText, project, &error) || !manifest.loadFromJson(mediaText, &error)) {}
    else if (const auto* timeline = project.activeTimeline()) {
        const auto utf8 = directory.u8string();
        return openTimeline(*timeline, manifest, std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size()),
                            project.colorSettings);
    } else error = "project has no active timeline";
    close(); impl_->notify(snapshot().generation, onOpenFailed, error); return false;
}
void VideoEngine::close() {
    impl_->stopAudio(); uint64_t generation;
    { std::lock_guard lock(impl_->mutex);
      generation = impl_->invalidateLocked(); impl_->state = Snapshot{}; impl_->state.generation = generation;
      Impl::Request command; command.generation = generation; impl_->request = command; }
    impl_->notify(generation, onStateChanged, State::Idle); impl_->notify(generation, onDurationChanged, 0.0);
    impl_->cv.notify_all(); impl_->wake();
}
void VideoEngine::shutdown() {
    if (!impl_) return;
    impl_->stopAudio();
    { std::lock_guard lock(impl_->mutex); impl_->stopping = true; impl_->invalidateLocked(); }
    impl_->lifetime->alive.store(false, std::memory_order_release);
    impl_->cv.notify_all();
    if (impl_->worker.joinable()) impl_->worker.join();
    std::lock_guard deviceLock(impl_->deviceMutex); impl_->device.reset();
}
void VideoEngine::play() {
    auto before = snapshot();
    if (before.state == State::Ended) { requestFrame(0, false, true); return; }
    if (before.state != State::Paused) return;
    { std::lock_guard lock(impl_->mutex);
      if (impl_->state.generation != before.generation) return;
      impl_->state.state = State::Playing; impl_->anchorClockLocked(impl_->state.currentTime); }
    impl_->startAudio(); impl_->notify(before.generation, onStateChanged, State::Playing);
    impl_->cv.notify_all(); impl_->wake();
}
void VideoEngine::pause() {
    auto before = snapshot(); if (before.state != State::Playing) return;
    impl_->stopAudio();
    { std::lock_guard lock(impl_->mutex); if (impl_->state.generation != before.generation) return; impl_->state.state = State::Paused; }
    impl_->notify(before.generation, onStateChanged, State::Paused); impl_->wake();
}
void VideoEngine::togglePlayPause() { if (snapshot().state == State::Playing) pause(); else play(); }
void VideoEngine::requestFrame(int64_t frame, bool scrub, bool resume) {
    impl_->stopAudio(); uint64_t generation;
    { std::lock_guard lock(impl_->mutex);
      if (!impl_->state.seekable || impl_->state.frameCount <= 0 || impl_->stopping) return;
      generation = impl_->invalidateLocked();
      frame = std::clamp(frame, int64_t{0}, impl_->state.frameCount - 1);
      impl_->state.state = scrub ? State::Scrubbing : State::Loading;
      Impl::Request command; command.kind = Impl::Request::Frame; command.generation = generation;
      command.frame = frame; command.scrub = scrub; command.resume = resume; command.hardware = impl_->hardware;
      impl_->request = std::move(command); }
    impl_->notify(generation, onStateChanged, scrub ? State::Scrubbing : State::Loading);
    impl_->cv.notify_all(); impl_->wake();
}
void VideoEngine::seek(double seconds) {
    const auto value = snapshot();
    if (!std::isfinite(seconds) || value.state == State::Scrubbing) return;
    requestFrame(static_cast<int64_t>(std::llround(std::clamp(seconds, 0.0, value.duration) * value.fps)),
                 false, value.state == State::Playing);
}
void VideoEngine::beginScrub() {
    const auto value = snapshot();
    if (!value.seekable || value.state == State::Loading || value.state == State::Scrubbing) return;
    impl_->stopAudio();
    { std::lock_guard lock(impl_->mutex); impl_->resumeAfterScrub = value.state == State::Playing; impl_->state.state = State::Scrubbing; }
    impl_->notify(value.generation, onStateChanged, State::Scrubbing); impl_->wake();
}
void VideoEngine::scrubToFrame(int64_t frame) { if (snapshot().state == State::Scrubbing) requestFrame(frame, true, false); }
void VideoEngine::endScrub(int64_t frame) {
    bool resume;
    { std::lock_guard lock(impl_->mutex); if (impl_->state.state != State::Scrubbing) return;
      resume = impl_->resumeAfterScrub; impl_->resumeAfterScrub = false; }
    requestFrame(frame, false, resume);
}
void VideoEngine::stepForward(int frames) { const auto s = snapshot(); seek(s.currentTime + frames / std::max(s.fps, 1.0)); }
void VideoEngine::stepBackward(int frames) { stepForward(-frames); }
void VideoEngine::goToStart() { seek(0.0); }
void VideoEngine::goToEnd() { const auto s = snapshot(); seek(std::max(0.0, (s.frameCount - 1) / std::max(s.fps, 1.0))); }

VideoEngine::Pump VideoEngine::pumpFrame(int& waitMs) {
    waitMs = 2;
    Impl::Queued packet;
    bool ended = false;
    double seconds = 0.0;
    uint64_t generation;
    { std::lock_guard lock(impl_->mutex);
      generation = impl_->state.generation;
      while (!impl_->videoQueue.empty() && impl_->videoQueue.front().frame.generation != generation) impl_->videoQueue.pop_front();
      if (impl_->videoQueue.empty()) {
          if (impl_->state.state != State::Playing && impl_->state.state != State::Loading) { waitMs = -1; return Pump::Paused; }
          if (impl_->state.state == State::Playing) ++impl_->diagnosticEmptyVideoPolls;
          return Pump::Wait;
      }
      auto& front = impl_->videoQueue.front();
      if (!front.urgent && impl_->state.state != State::Playing) { waitMs = -1; return Pump::Paused; }
      if (impl_->discardAudio || !impl_->audioOutputEnabled) {
          impl_->audioQueue.clear(); impl_->currentAudio.reset(); impl_->audioOffset = 0;
      }
      const double master = impl_->audioClock
          ? static_cast<double>(impl_->audioPosition.load(std::memory_order_relaxed)) / impl_->audioSpec.sampleRate
          : impl_->clockSecondsLocked();
      const double due = front.eof ? impl_->state.duration
          : front.frame.targetPosition * front.frame.targetTimeBase.toDouble();
      // Do not let the video's early-presentation tolerance stop the audio
      // device before its last PCM slice has actually been consumed.
      if (front.eof && impl_->audioClock && (impl_->currentAudio || !impl_->audioQueue.empty())) {
          waitMs = 1; return Pump::Wait;
      }
      if (!front.urgent && due - master > 0.002) {
          waitMs = std::max(1, static_cast<int>((due - master) * 1000)); return Pump::Wait;
      }
      if (!front.eof && !front.urgent) {
          ++impl_->diagnosticPresented;
          const double lateMs = std::max(0.0, (master - due) * 1000.0);
          impl_->diagnosticMaxLateMs = std::max(impl_->diagnosticMaxLateMs, lateMs);
          if (lateMs > 1000.0 / impl_->state.fps) ++impl_->diagnosticLateFrames;
      }
      packet = std::move(front); impl_->videoQueue.pop_front();
      ended = packet.eof; seconds = due;
      impl_->state.currentTime = seconds;
      if (ended) { impl_->state.state = State::Ended; waitMs = -1; } else waitMs = 0;
    }
    impl_->cv.notify_all();
    if (!ended && isCurrentGeneration(generation) && onFrameDecoded) onFrameDecoded(std::move(packet.frame));
    impl_->notify(generation, onPositionChanged, seconds);
    if (ended) {
        impl_->stopAudio(); impl_->notify(generation, onStateChanged, State::Ended);
        impl_->notify(generation, onEndOfStream); return Pump::Ended;
    }
    return Pump::Presented;
}

} // namespace heisenberg
