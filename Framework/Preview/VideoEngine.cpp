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
    Rational rate{24, 1};
    double duration = 0.0, videoEnd = -1.0, lastImageEnd = -1.0;
    bool seekable = false, eof = false, resamplerDrained = false;
    double selectedSeconds = -1.0;
    std::deque<std::pair<double, std::shared_ptr<VideoFrame>>> images;
    std::vector<std::shared_ptr<AudioFrame>> sound;
    AudioResampler resampler;
    AudioSpec outputSpec;
    double audioEnd = -1.0;
    int64_t inferredAudioPosition = 0;
    double inferredVideoSeconds = 0.0;

    bool open(const std::string& path, bool hardware, const AudioSpec& output,
              bool wantVideo, std::string& error) {
        outputSpec = output;
        demux = std::make_unique<demuxer::DemuxContext>();
        if (demux->open(path) < 0) { error = "Failed to open file: " + path; return false; }
        duration = std::max(0.0, demux->duration());
        seekable = demux->seekable();
        for (const auto& stream : demux->streams()) {
            if (wantVideo && stream.isVideo() && !stream.attachedPicture && !videoStream) videoStream = &stream;
            if (stream.isAudio() && !audioStream) audioStream = &stream;
        }
        const bool playable = std::any_of(demux->streams().begin(), demux->streams().end(),
            [](const auto& stream) { return stream.isAudio() || (stream.isVideo() && !stream.attachedPicture); });
        if (!playable) { error = "No playable video or audio stream found"; return false; }
        decoder::DecoderConfig config;
        config.preferred = hardware ? decoder::DecoderBackend::D3D11 : decoder::DecoderBackend::Software;
        config.allowFallback = hardware;
        if (videoStream) {
            if (videoStream->codec.fpsNum() > 0 && videoStream->codec.fpsDen() > 0)
                rate = Rational(videoStream->codec.fpsNum(), videoStream->codec.fpsDen());
            if (videoStream->durationKnown)
                videoEnd = videoStream->codec.toSeconds(videoStream->startTime - videoStream->originTime + videoStream->duration);
            video = std::make_unique<decoder::DecoderContext>();
            if (video->open(*videoStream, config) < 0) { error = "Failed to create video decoder"; return false; }
        }
        // Separate video and audio contribution sessions avoid retaining a
        // video's surfaces while decoding independent audio lookahead/tails.
        if (audioStream && !wantVideo) {
            audio = std::make_unique<decoder::DecoderContext>();
            if (audio->open(*audioStream, config) < 0) {
                audio.reset();
                if (!video) { error = "Failed to create audio decoder"; return false; }
            }
        }
        return true;
    }
    void appendSound(std::shared_ptr<AudioFrame> converted) {
        if (!converted) return;
        if (!converted->hasPosition()) converted->setPosition(inferredAudioPosition);
        inferredAudioPosition = converted->position() + converted->samples();
        audioEnd = static_cast<double>(inferredAudioPosition) / outputSpec.sampleRate;
        sound.push_back(std::move(converted));
    }
    void prune(double seconds) {
        const int64_t before = static_cast<int64_t>(std::floor(seconds * outputSpec.sampleRate)) - 2;
        sound.erase(std::remove_if(sound.begin(), sound.end(), [&](const auto& frame) {
            return frame->position() + frame->samples() < before;
        }), sound.end());
        while (images.size() > 1 && images.front().first + 0.000001 < seconds) images.pop_front();
        // Demuxers with highly skewed interleave must not retain unbounded
        // decoded references. Normal lookahead is just one presentation slice.
        while (images.size() > kVideoCapacity) images.pop_back();
        if (sound.size() > 256) sound.resize(256);
    }
    void pull(uint64_t generation, double seconds) {
        if (video) for (;;) {
            auto item = video->pull(generation);
            auto image = item.videoFrame();
            if (!image) break;
            const auto native = image->native();
            const double position = hasFrameTimestamp(*native) ? frameTimeSeconds(*native) : inferredVideoSeconds;
            const double span = native->duration > 0 ? native->duration * av_q2d(native->time_base) : 1.0 / rate.toDouble();
            inferredVideoSeconds = position + span;
            lastImageEnd = inferredVideoSeconds;
            images.emplace_back(position, std::move(image));
            prune(seconds);
        }
        if (audio) for (;;) {
            auto item = audio->pull(generation);
            auto frame = item.audioFrame();
            if (!frame) break;
            appendSound(resampler.convert(*frame, outputSpec));
            prune(seconds);
        }
        if (eof && !resamplerDrained) {
            while (auto frame = resampler.drain()) appendSound(std::move(frame));
            resamplerDrained = true;
            prune(seconds);
        }
    }
    std::shared_ptr<VideoFrame> decode(double seconds, double audioSeconds,
                                      uint64_t generation, const std::atomic<uint64_t>& active) {
        if (!video && !audio) { selectedSeconds = seconds; return {}; }
        const bool needSeek = (selectedSeconds < 0.0 ? seconds > 2.0
            : (seconds + 0.000001 < selectedSeconds || seconds - selectedSeconds > 2.0));
        if (needSeek && seekable) {
            const double preroll = std::max(0.0, seconds - 2.0);
            const int streamIndex = videoStream ? videoStream->index : audioStream->index;
            if (demux->seek(preroll, streamIndex, 1) < 0) return {};
            if (video) video->flush();
            if (audio) audio->flush();
            resampler.reset();
            images.clear(); sound.clear(); eof = false; resamplerDrained = false;
            audioEnd = lastImageEnd = -1.0;
            inferredVideoSeconds = preroll;
            inferredAudioPosition = static_cast<int64_t>(preroll * outputSpec.sampleRate);
        }
        for (;;) {
            if (active.load(std::memory_order_acquire) != generation) return {};
            pull(generation, seconds);
            prune(seconds);
            const bool videoReady = !video || (videoEnd >= 0 && seconds >= videoEnd)
                || (!images.empty() && images.back().first + 0.000001 >= seconds);
            const bool audioReady = !audio || audioEnd >= audioSeconds;
            if ((videoReady && audioReady) || eof) break;
            auto packet = demux->read(generation);
            if (packet.type == MediaFrameType::Packet) {
                if (video && packet.metadata.streamIndex == videoStream->index) video->push(packet);
                else if (audio && packet.metadata.streamIndex == audioStream->index) audio->push(packet);
            } else {
                if (video) video->push(MediaFrame::eof(generation));
                if (audio) audio->push(MediaFrame::eof(generation));
                eof = true;
            }
        }
        pull(generation, seconds);
        prune(seconds);
        selectedSeconds = seconds;
        if (images.empty() || (videoEnd >= 0 && seconds >= videoEnd)
            || (eof && lastImageEnd >= 0 && seconds >= lastImageEnd)) return {};
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
    bool audioClock = false, audioOutputEnabled = true, discardAudio = false;
    bool manualClock = false;
    double manualNow = 0.0, manualStart = 0.0;
    Clock::time_point wallStart = Clock::now();
    double wallOffset = 0.0;
    std::thread worker;
    // The fields below are touched by the decode worker only.
    CompositionPlan plan;
    bool sourceMode = false;
    std::unordered_map<std::string, std::unique_ptr<DecoderSession>> sessions;
    // Failed opens are source failures, shared across contribution identities.
    // Keep only currently active paths and reset on generation/plan changes.
    std::unordered_map<std::string, std::string> failedPaths;
    int64_t nextFrame = 0;
    bool ready = false, eofQueued = false;
    double sourceDuration = 0.0;
    bool sourceHasAudio = false;
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
    DecoderSession* session(const std::string& identity, const std::string& path,
                            bool wantVideo, bool hardwareDecode, uint64_t generation) {
        if (path.empty()) return nullptr;
        if (failedPaths.find(path) != failedPaths.end()) return nullptr;
        auto found = sessions.find(identity);
        if (found != sessions.end()) return found->second.get();
        auto created = std::make_unique<DecoderSession>();
        std::string error;
        if (!created->open(path, hardwareDecode, plan.settings.audioSpec(), wantVideo, error)) {
            LOG_WARN("VideoEngine: {}", error);
            failedPaths.emplace(path, error);
            { std::lock_guard lock(mutex);
              if (state.generation == generation) {
                  const std::string warning = "unavailable media: " + path + " (" + error + ")";
                  if (std::find(state.warnings.begin(), state.warnings.end(), warning) == state.warnings.end())
                      state.warnings.push_back(warning);
              } }
            return nullptr;
        }
        if (active.load(std::memory_order_acquire) != generation) return nullptr;
        auto* result = created.get();
        sessions.emplace(identity, std::move(created));
        return result;
    }
    bool execute(int64_t frame, uint64_t generation, bool urgent, bool hardwareDecode) {
        DecodedFrame output;
        output.settings = plan.settings;
        output.targetPosition = frame;
        output.targetTimeBase = plan.settings.timeBase();
        output.generation = generation; output.sourceMode = sourceMode;
        std::vector<AudioMixer::Input> inputs;
        std::vector<std::string> used, usedPaths;
        int samples = static_cast<int>(plan.settings.audioSamplesForFrame(frame));
        if (sourceMode) samples = static_cast<int>(std::max<int64_t>(0, std::min<int64_t>(samples,
            static_cast<int64_t>(std::llround(sourceDuration * plan.settings.audioSpec().sampleRate))
                - plan.settings.audioSamplePosition(frame))));
        auto contribution = [&](const CompositionSample& sample, const CompositionClip& clip,
                                const std::string& prefix, bool wantVideo) {
            const std::string identity = prefix + std::to_string(sample.clipIndex) + ":" + clip.id + ":" + clip.sourcePath;
            used.push_back(identity); usedPaths.push_back(clip.sourcePath);
            auto* decoder = session(identity, clip.sourcePath, wantVideo, hardwareDecode, generation);
            if (!decoder) return;
            const double end = sample.sourceSeconds + static_cast<double>(samples)
                / plan.settings.audioSpec().sampleRate * sample.speed;
            auto image = decoder->decode(sample.sourceSeconds, end, generation, active);
            if (wantVideo && !sample.hidden) output.source = std::move(image);
            if (decoder->audio) {
                AudioMixer::Input input;
                input.source = &decoder->sound; input.sourceSeconds = sample.sourceSeconds;
                input.speed = sample.speed; input.gain = sample.gain; input.muted = sample.muted;
                input.clipOffsetSeconds = plan.settings.secondsFromFrame(frame - clip.startFrame);
                input.clipDurationSeconds = plan.settings.secondsFromFrame(clip.durationFrames);
                input.fadeInSeconds = plan.settings.secondsFromFrame(clip.fadeInFrames);
                input.fadeOutSeconds = plan.settings.secondsFromFrame(clip.fadeOutFrames);
                inputs.push_back(input);
            }
        };
        if (sourceMode) {
            CompositionSample sample; sample.sourceSeconds = plan.settings.secondsFromFrame(frame);
            const auto& clip = plan.clips.front();
            contribution(sample, clip, "video:", true);
            if (sourceHasAudio) contribution(sample, clip, "original-audio:", false);
        } else {
            if (auto sample = plan.videoSampleAt(frame)) {
                const auto& clip = plan.clips[sample->clipIndex];
                contribution(*sample, clip, "video:", true);
                contribution(*sample, clip, "original-audio:", false);
            }
            if (auto sample = plan.audioSampleAt(frame))
                contribution(*sample, plan.audioClips[sample->clipIndex], "independent-audio:", false);
        }
        if (active.load(std::memory_order_acquire) != generation) return false;
        output.blank = !output.source;
        const auto sound = AudioMixer::render(plan.settings.audioSpec(), samples,
            plan.settings.audioSamplePosition(frame), inputs);
        // Observe exactly the prepared post-mix PCM, on the sole decode worker.
        if (sound && owner->onAudioPrepared && active.load(std::memory_order_acquire) == generation)
            owner->onAudioPrepared(sound, generation);
        { std::lock_guard lock(mutex);
          if (state.generation != generation) return false;
          videoQueue.push_back({std::move(output), urgent, false});
          if (sound && audioOutputEnabled && !discardAudio) audioQueue.push_back({sound, generation}); }
        // Same path at different source positions is never a shared session.
        for (auto it = sessions.begin(); it != sessions.end();) {
            if (std::find(used.begin(), used.end(), it->first) == used.end()) it = sessions.erase(it);
            else ++it;
        }
        for (auto it = failedPaths.begin(); it != failedPaths.end();) {
            if (std::find(usedPaths.begin(), usedPaths.end(), it->first) == usedPaths.end()) it = failedPaths.erase(it);
            else ++it;
        }
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
                if (command->kind == Request::Close) { sessions.clear(); failedPaths.clear(); ready = false; continue; }
                if (command->kind == Request::Source || command->kind == Request::Timeline) {
                    sessions.clear(); failedPaths.clear(); plan = std::move(command->plan);
                    sourceMode = command->kind == Request::Source;
                    ready = false;
                    if (sourceMode) {
                        plan.settings = TimelineSettings::hd1080p24();
                        auto* decoder = session("video:0::" + command->path, command->path, true, decodeHardware, generation);
                        if (!decoder) { fail(generation, "Failed to open media: " + command->path); continue; }
                        sourceDuration = decoder->duration;
                        sourceHasAudio = decoder->audioStream != nullptr;
                        VideoSpec spec = plan.settings.videoSpec();
                        if (decoder->videoStream) {
                            spec.width = decoder->videoStream->codec.width();
                            spec.height = decoder->videoStream->codec.height();
                        }
                        spec.frameRate = decoder->rate; spec.timeBase = decoder->rate.flipped();
                        plan.settings = TimelineSettings(spec, plan.settings.audioSpec());
                        plan.frameCount = static_cast<int64_t>(std::ceil(decoder->duration * decoder->rate.toDouble()));
                        CompositionClip clip; clip.sourcePath = command->path; clip.durationFrames = plan.frameCount;
                        plan.clips = {clip};
                        if (plan.frameCount <= 0) { fail(generation, "Media duration is empty"); continue; }
                    }
                    { std::lock_guard lock(mutex);
                      if (state.generation != generation) continue;
                      state.duration = sourceMode ? sourceDuration : plan.settings.secondsFromFrame(plan.frameCount);
                      state.fps = plan.settings.fps(); state.frameCount = plan.frameCount;
                      state.seekable = sourceMode ? sessions.at("video:0::" + command->path)->seekable : plan.frameCount > 0;
                      state.hasVideo = sourceMode ? sessions.at("video:0::" + command->path)->videoStream != nullptr : !plan.clips.empty();
                      state.audioOnly = !state.hasVideo && (sourceMode ? sourceHasAudio : !plan.audioClips.empty());
                      audioSpec = plan.settings.audioSpec(); }
                    notify(generation, owner->onDurationChanged, sourceMode ? sourceDuration : plan.settings.secondsFromFrame(plan.frameCount));
                }
                // Reset even nearby seek/scrub requests: generation changes must
                // not retain decoder delay, cached images or resampler history.
                if (command->kind == Request::Frame) { sessions.clear(); failedPaths.clear(); }
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
            if (execute(nextFrame, generation, false, decodeHardware)) ++nextFrame;
        }
        sessions.clear(); failedPaths.clear();
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
