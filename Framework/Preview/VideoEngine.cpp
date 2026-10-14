#include "VideoEngine.hpp"
#include <Compositing/CompositionBuilder.hpp>
#include <Common/FrameTime.hpp>
#include <Common/MediaFrame.hpp>
#include <Common/Stream.hpp>
#include <Models/ProjectCodec.hpp>
#include <Models/ProjectFile.hpp>
#include <MultiMedia/Audio/AudioMixer.hpp>
#include <MultiMedia/Audio/AudioResampler.hpp>
#include <MultiMedia/Audio/Output/AudioDevice.hpp>
#include <MultiMedia/DecoderContext.hpp>
#include <MultiMedia/DemuxContext.hpp>
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
#include <unordered_map>
extern "C" {
#include <libavutil/frame.h>
}

namespace heisenberg {
namespace {
using Clock = std::chrono::steady_clock;
constexpr size_t kVideoCapacity = 12;
constexpr size_t kAudioCapacity = 48;

// Decoder sessions are not producers or execution graphs. They are private
// resources of the one engine worker, shared by source and timeline execution.
struct DecoderSession {
    std::unique_ptr<demuxer::DemuxContext> demux;
    std::unique_ptr<decoder::DecoderContext> video, audio;
    const Stream* videoStream = nullptr;
    const Stream* audioStream = nullptr;
    Rational rate{25, 1};
    double duration = 0.0;
    bool seekable = false, eof = false;
    double selectedSeconds = -1.0;
    std::deque<std::pair<double, std::shared_ptr<VideoFrame>>> images;
    std::vector<std::shared_ptr<AudioFrame>> sound;
    AudioResampler resampler;
    AudioSpec outputSpec;
    double audioEnd = -1.0;
    int64_t inferredAudioPosition = 0;
    double inferredVideoSeconds = 0.0;

    bool open(const std::string& path, bool hardware, const AudioSpec& output, std::string& error) {
        outputSpec = output;
        demux = std::make_unique<demuxer::DemuxContext>();
        if (demux->open(path) < 0) { error = "Failed to open file: " + path; return false; }
        duration = std::max(0.0, demux->duration());
        seekable = demux->seekable();
        for (const auto& stream : demux->streams()) {
            if (stream.isVideo() && !videoStream) videoStream = &stream;
            if (stream.isAudio() && !audioStream) audioStream = &stream;
        }
        if (!videoStream) { error = "No video stream found"; return false; }
        if (videoStream->codec.fpsNum() > 0 && videoStream->codec.fpsDen() > 0)
            rate = Rational(videoStream->codec.fpsNum(), videoStream->codec.fpsDen());
        decoder::DecoderConfig config;
        config.preferred = hardware ? decoder::DecoderBackend::D3D11 : decoder::DecoderBackend::Software;
        config.allowFallback = hardware;
        video = std::make_unique<decoder::DecoderContext>();
        // Device provisioning belongs to the render thread. A missing hardware
        // device is handled by DecoderContext's software fallback.
        if (video->open(*videoStream, config) < 0) { error = "Failed to create video decoder"; return false; }
        if (audioStream) {
            audio = std::make_unique<decoder::DecoderContext>();
            if (audio->open(*audioStream, config) < 0) audio.reset();
        }
        return true;
    }
    void pull(uint64_t generation) {
        for (;;) {
            auto item = video->pull(generation);
            auto image = item.videoFrame();
            if (!image) break;
            const auto native = image->native();
            double seconds = hasFrameTimestamp(*native) ? frameTimeSeconds(*native) : inferredVideoSeconds;
            inferredVideoSeconds = seconds + 1.0 / rate.toDouble();
            images.emplace_back(seconds, std::move(image));
        }
        if (!audio) return;
        for (;;) {
            auto item = audio->pull(generation);
            auto frame = item.audioFrame();
            if (!frame) break;
            auto converted = resampler.convert(*frame, outputSpec);
            if (!converted) continue;
            if (!converted->hasPosition()) converted->setPosition(inferredAudioPosition);
            inferredAudioPosition = converted->position() + converted->samples();
            audioEnd = static_cast<double>(inferredAudioPosition) / outputSpec.sampleRate;
            sound.push_back(std::move(converted));
        }
    }
    std::shared_ptr<VideoFrame> decode(double seconds, double audioSeconds,
                                      uint64_t generation, const std::atomic<uint64_t>& active) {
        const bool needSeek = (!images.empty() && images.front().first > seconds + 1.0 / rate.toDouble())
            || (selectedSeconds < 0.0 ? seconds > 2.0
                : (seconds + 0.000001 < selectedSeconds || seconds - selectedSeconds > 2.0));
        if (needSeek && seekable) {
            const double preroll = std::max(0.0, seconds - 2.0);
            if (demux->seek(preroll, videoStream->index, 1) < 0) return {};
            video->flush();
            if (audio) audio->flush();
            resampler.reset();
            images.clear(); sound.clear(); eof = false; audioEnd = -1.0;
            inferredVideoSeconds = preroll;
            inferredAudioPosition = static_cast<int64_t>(preroll * outputSpec.sampleRate);
        }
        for (;;) {
            if (active.load(std::memory_order_acquire) != generation) return {};
            pull(generation);
            const int64_t beforeSample = static_cast<int64_t>(seconds * outputSpec.sampleRate) - 2;
            sound.erase(std::remove_if(sound.begin(), sound.end(), [&](const auto& frame) {
                return frame->position() + frame->samples() < beforeSample;
            }), sound.end());
            // Release preroll references as decoding progresses. Retaining an
            // entire seek preroll can exhaust a fixed D3D11 surface pool.
            while (images.size() > 1 && images.front().first + 0.000001 < seconds) images.pop_front();
            const bool videoReady = !images.empty() && images.back().first + 0.000001 >= seconds;
            const bool audioReady = !audio || audioEnd >= audioSeconds;
            if ((videoReady && audioReady) || eof) break;
            auto packet = demux->read(generation);
            if (packet.type == MediaFrameType::Packet) {
                if (packet.metadata.streamIndex == videoStream->index) video->push(packet);
                else if (audio && packet.metadata.streamIndex == audioStream->index) audio->push(packet);
            } else {
                video->push(MediaFrame::eof(generation));
                if (audio) audio->push(MediaFrame::eof(generation));
                eof = true;
            }
        }
        pull(generation);
        while (images.size() > 1 && images.front().first + 0.000001 < seconds) images.pop_front();
        if (images.empty() || (duration > 0 && seconds >= duration)) return {};
        selectedSeconds = seconds;
        const int64_t before = static_cast<int64_t>(seconds * outputSpec.sampleRate) - 2;
        sound.erase(std::remove_if(sound.begin(), sound.end(), [&](const auto& frame) {
            return frame->position() + frame->samples() < before;
        }), sound.end());
        return images.front().second;
    }
};
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
    bool audioClock = false;
    Clock::time_point wallStart = Clock::now();
    double wallOffset = 0.0;
    std::thread worker;
    // The fields below are touched by the decode worker only.
    CompositionPlan plan;
    bool sourceMode = false;
    std::unordered_map<std::string, std::unique_ptr<DecoderSession>> sessions;
    int64_t nextFrame = 0;
    bool ready = false, eofQueued = false;

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
        cv.notify_all();
    }
    void startAudio() {
        std::lock_guard deviceLock(deviceMutex);
        AudioSpec spec;
        { std::lock_guard lock(mutex);
          if (state.state != State::Playing || stopping) return;
          spec = audioSpec; }
        if (!spec.valid()) return;
        if (!device) {
            auto candidate = std::make_unique<renderer::AudioDevice>();
            if (!candidate->initialize(spec, [this](float* out, uint32_t n) { audioData(out, n); })) return;
            device = std::move(candidate);
        }
        const bool started = device->start();
        std::lock_guard lock(mutex);
        audioClock = started;
    }
    uint64_t invalidateLocked() {
        const auto generation = active.fetch_add(1, std::memory_order_acq_rel) + 1;
        lifetime->generation.store(generation, std::memory_order_release);
        state.generation = generation;
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
    DecoderSession* session(const std::string& path, bool hardwareDecode, uint64_t generation) {
        auto found = sessions.find(path);
        if (found != sessions.end()) return found->second.get();
        auto created = std::make_unique<DecoderSession>();
        std::string error;
        if (!created->open(path, hardwareDecode, plan.settings.audioSpec(), error)) {
            LOG_WARN("VideoEngine: {}", error);
            { std::lock_guard lock(mutex);
              if (state.generation == generation) state.warnings.push_back("unavailable media: " + path + " (" + error + ")"); }
            sessions.emplace(path, nullptr); return nullptr;
        }
        if (active.load(std::memory_order_acquire) != generation) return nullptr;
        auto* result = created.get();
        sessions.emplace(path, std::move(created));
        return result;
    }
    bool execute(int64_t frame, uint64_t generation, bool urgent, bool hardwareDecode) {
        DecodedFrame output;
        output.settings = plan.settings;
        output.targetPosition = frame;
        output.targetTimeBase = plan.settings.timeBase();
        output.generation = generation; output.sourceMode = sourceMode;
        double seconds = plan.settings.secondsFromFrame(frame), speed = 1.0, gain = 1.0;
        bool muted = false, hidden = false;
        std::string path;
        if (sourceMode) path = plan.clips.front().sourcePath;
        else if (auto sample = plan.sampleAt(frame)) {
            path = plan.clips[sample->clipIndex].sourcePath;
            seconds = sample->sourceSeconds; speed = sample->speed; gain = sample->gain;
            muted = sample->muted; hidden = sample->hidden;
        }
        auto* decoder = path.empty() ? nullptr : session(path, hardwareDecode, generation);
        const double audioEnd = seconds + plan.settings.secondsFromFrame(1) * speed;
        if (decoder) output.source = decoder->decode(seconds, audioEnd, generation, active);
        if (active.load(std::memory_order_acquire) != generation) return false;
        output.blank = hidden || !output.source;
        if (hidden) output.source.reset();
        const auto sound = AudioMixer::render(plan.settings.audioSpec(),
            static_cast<int>(plan.settings.audioSamplesForFrame(frame)),
            plan.settings.audioSamplePosition(frame), decoder ? decoder->sound : std::vector<std::shared_ptr<AudioFrame>>{},
            seconds, speed, gain, muted || !decoder);
        { std::lock_guard lock(mutex);
          if (state.generation != generation) return false;
          videoQueue.push_back({std::move(output), urgent, false});
          if (sound) audioQueue.push_back({sound, generation}); }
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
                generation = command->generation;
                decodeHardware = command->hardware;
                if (command->kind == Request::Close) { sessions.clear(); ready = false; continue; }
                if (command->kind == Request::Source || command->kind == Request::Timeline) {
                    sessions.clear(); plan = std::move(command->plan);
                    sourceMode = command->kind == Request::Source;
                    ready = false;
                    if (sourceMode) {
                        plan.settings = TimelineSettings::hd1080p24();
                        auto* decoder = session(command->path, decodeHardware, generation);
                        if (!decoder) { fail(generation, "Failed to open media: " + command->path); continue; }
                        VideoSpec spec;
                        spec.width = decoder->videoStream->codec.width();
                        spec.height = decoder->videoStream->codec.height();
                        spec.frameRate = decoder->rate; spec.timeBase = decoder->rate.flipped();
                        plan.settings = TimelineSettings(spec, plan.settings.audioSpec());
                        plan.frameCount = static_cast<int64_t>(std::ceil(decoder->duration * decoder->rate.toDouble()));
                        CompositionClip clip; clip.sourcePath = command->path; clip.durationFrames = plan.frameCount;
                        plan.clips = {clip};
                        if (plan.frameCount <= 0) { fail(generation, "Media duration is empty"); continue; }
                    }
                    { std::lock_guard lock(mutex);
                      if (state.generation != generation) continue;
                      state.duration = plan.settings.secondsFromFrame(plan.frameCount);
                      state.fps = plan.settings.fps(); state.frameCount = plan.frameCount;
                      state.seekable = sourceMode ? sessions.at(command->path)->seekable : plan.frameCount > 0;
                      audioSpec = plan.settings.audioSpec(); }
                    notify(generation, owner->onDurationChanged, plan.settings.secondsFromFrame(plan.frameCount));
                }
                nextFrame = command->frame; eofQueued = false;
                if (plan.frameCount <= 0) {
                    { std::lock_guard lock(mutex); if (state.generation == generation) state.state = State::Paused; }
                    notify(generation, owner->onStateChanged, State::Paused);
                    continue;
                }
                if (!execute(nextFrame, generation, true, decodeHardware)) continue;
                ++nextFrame; ready = true;
                const auto nextState = command->scrub ? State::Scrubbing : State::Paused;
                { std::lock_guard lock(mutex);
                  if (state.generation != generation) continue;
                  state.state = nextState;
                  state.currentTime = plan.settings.secondsFromFrame(command->frame);
                  wallOffset = state.currentTime; wallStart = Clock::now();
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
            if (execute(nextFrame, generation, false, decodeHardware)) ++nextFrame;
        }
        sessions.clear();
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
    for (const auto& warning : plan.warnings) LOG_WARN("Composition: {}", warning);
    impl_->stopAudio(); uint64_t generation;
    { std::lock_guard lock(impl_->mutex);
      if (impl_->stopping) return false;
      generation = impl_->invalidateLocked(); impl_->state = Snapshot{};
      impl_->state.generation = generation; impl_->state.state = State::Loading;
      Impl::Request command; command.kind = Impl::Request::Timeline; command.plan = std::move(plan);
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
        return openTimeline(*timeline, manifest, std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size()));
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
      impl_->state.state = State::Playing; impl_->wallOffset = impl_->state.currentTime; impl_->wallStart = Clock::now(); }
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
          return Pump::Wait;
      }
      auto& front = impl_->videoQueue.front();
      if (!front.urgent && impl_->state.state != State::Playing) { waitMs = -1; return Pump::Paused; }
      const double master = impl_->audioClock
          ? static_cast<double>(impl_->audioPosition.load(std::memory_order_relaxed)) / impl_->audioSpec.sampleRate
          : impl_->wallOffset + std::chrono::duration<double>(Clock::now() - impl_->wallStart).count();
      const double due = front.eof ? impl_->state.duration
          : front.frame.targetPosition * front.frame.targetTimeBase.toDouble();
      if (!front.urgent && due - master > 0.002) {
          waitMs = std::max(1, static_cast<int>((due - master) * 1000)); return Pump::Wait;
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
