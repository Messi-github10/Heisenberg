#include "CompositionExecutor.hpp"
#include <Common/MediaMath.hpp>
#include <Common/MediaFrame.hpp>
#include <Common/Packet.hpp>
#include <Common/Stream.hpp>
#include <MultiMedia/Audio/AudioResampler.hpp>
#include <MultiMedia/DecoderContext.hpp>
#include <MultiMedia/DemuxContext.hpp>
#include <Utiles/Logger.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <limits>
#include <unordered_map>

extern "C" {
#include <libavutil/error.h>
#include <libavutil/frame.h>
}

namespace heisenberg {

namespace {
constexpr size_t kVideoPacketCapacity = 256;
constexpr size_t kVideoPacketBytes = 16 * 1024 * 1024;
constexpr size_t kAudioCapacity = 256;
constexpr int64_t kMixBlock = 2048;
constexpr int64_t kMaxAudioRequest = 1024 * 1024;
using Context = CompositionExecutor::Context;
using Status = CompositionStatus;

bool cancelled(const Context& context) noexcept {
    try {
        return context.cancelled && context.cancelled();
    } catch (...) {
        return true;
    }
}

std::string ffmpegError(int code) {
    char text[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, text, sizeof(text));
    return text;
}

struct InterruptScope {
    demuxer::DemuxContext& demux;
    InterruptScope(demuxer::DemuxContext& value, const Context& context) : demux(value) {
        demux.setInterrupt([&context] { return cancelled(context); });
    }
    ~InterruptScope() {
        demux.setInterrupt({});
    }
};

struct DecoderSession {
    std::unique_ptr<demuxer::DemuxContext> demux;
    std::unique_ptr<decoder::DecoderContext> video, audio;
    const Stream* videoStream = nullptr;
    const Stream* audioStream = nullptr;
    Rational rate{24, 1};
    double duration = 0.0, videoEnd = -1.0, lastImageEnd = -1.0;
    bool seekable = false, eof = false, resamplerDrained = false;
    bool videoDrainSent = false, videoDrained = false, videoCoverageLost = false;
    double selectedSeconds = -1.0;
    double decodedVideoSeconds = -std::numeric_limits<double>::infinity();
    std::optional<std::pair<double, std::shared_ptr<VideoFrame>>> lookaheadVideo;
    std::deque<std::pair<double, std::shared_ptr<VideoFrame>>> images;
    std::deque<MediaFrame> pendingVideo;
    size_t pendingVideoBytes = 0;
    std::vector<std::shared_ptr<AudioFrame>> sound;
    AudioResampler resampler;
    AudioSpec outputSpec;
    int64_t audioEnd = std::numeric_limits<int64_t>::min();
    int64_t discardedAudioBefore = std::numeric_limits<int64_t>::min();
    int64_t inferredAudioPosition = 0, expectedInputPosition = 0;
    bool inputPositionKnown = false;
    double inferredVideoSeconds = 0.0;
    std::string sourcePath, error;
    bool hardwareDecode = false, wantsVideo = false;
    std::string diagnosticIdentity;
    uint64_t diagnosticCalls = 0, diagnosticReopens = 0, diagnosticLostPictures = 0, diagnosticLostAudio = 0;
    uint64_t diagnosticReads = 0, diagnosticVideoFrames = 0, diagnosticAudioFrames = 0;
    double diagnosticDecodeMs = 0.0, diagnosticMaxDecodeMs = 0.0, diagnosticOpenMs = 0.0;
    double diagnosticRestartSource = -1.0, diagnosticRestartSelected = -1.0;
    double diagnosticRestartDecodedVideo = -1.0;
    int64_t diagnosticRestartFirst = 0, diagnosticRestartDiscardedAudio = 0;
    std::chrono::steady_clock::time_point diagnosticLast{};

    Status open(const std::string& path, bool hardware, const AudioSpec& output,
                bool wantVideo, const Context& context) {
        const auto diagnosticStart = std::chrono::steady_clock::now();
        sourcePath = path;
        hardwareDecode = hardware;
        wantsVideo = wantVideo;
        video.reset();
        audio.reset();
        videoStream = nullptr;
        audioStream = nullptr;
        resampler.reset();
        sound.clear();
        images.clear();
        pendingVideo.clear();
        lookaheadVideo.reset();
        pendingVideoBytes = 0;
        rate = Rational(24, 1);
        duration = 0.0;
        videoEnd = lastImageEnd = selectedSeconds = -1.0;
        decodedVideoSeconds = -std::numeric_limits<double>::infinity();
        inferredVideoSeconds = 0.0;
        audioEnd = discardedAudioBefore = std::numeric_limits<int64_t>::min();
        inferredAudioPosition = expectedInputPosition = 0;
        inputPositionKnown = false;
        eof = resamplerDrained = videoDrainSent = videoDrained = videoCoverageLost = false;
        error.clear();
        outputSpec = output;
        demux = std::make_unique<demuxer::DemuxContext>();
        InterruptScope interrupt(*demux, context);
        if (cancelled(context)) return Status::Cancelled;
        const int opened = demux->open(path);
        if (opened < 0) {
            error = "Failed to open file: " + path + " (" + ffmpegError(opened) + ")";
            return cancelled(context) || opened == AVERROR_EXIT ? Status::Cancelled : Status::Failure;
        }
        duration = std::max(0.0, demux->duration());
        seekable = demux->seekable();
        for (const auto& stream : demux->streams()) {
            if (stream.isVideo() && !stream.attachedPicture && !videoStream) videoStream = &stream;
            if (stream.isAudio() && !audioStream) audioStream = &stream;
        }
        decoder::DecoderConfig config;
        config.preferred = hardware ? decoder::DecoderBackend::D3D11 : decoder::DecoderBackend::Software;
        config.allowFallback = hardware;
        if (videoStream && wantVideo) {
            if (videoStream->codec.fpsNum() > 0 && videoStream->codec.fpsDen() > 0)
                rate = Rational(videoStream->codec.fpsNum(), videoStream->codec.fpsDen());
            if (videoStream->durationKnown)
                videoEnd = videoStream->codec.toSeconds(videoStream->startTime - videoStream->originTime + videoStream->duration);
            video = std::make_unique<decoder::DecoderContext>();
            if (video->open(*videoStream, config) < 0) {
                error = "Failed to create video decoder: " + path;
                video.reset();
            }
        }
        if (audioStream) {
            audio = std::make_unique<decoder::DecoderContext>();
            if (audio->open(*audioStream, config) < 0) {
                if (error.empty()) error = "Failed to create audio decoder: " + path;
                audio.reset();
            }
        }
        if (cancelled(context)) return Status::Cancelled;
        if (!video && !audio) {
            if (error.empty()) error = "No usable video or audio decoder: " + path;
            return Status::Failure;
        }
        error.clear();
        diagnosticOpenMs += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - diagnosticStart).count();
        return Status::Succeeded;
    }

    void reportDecode(double elapsedMs, std::optional<double> seconds, int64_t first, int64_t last,
                      const Context& context, Status status) {
        ++diagnosticCalls;
        diagnosticDecodeMs += elapsedMs;
        diagnosticMaxDecodeMs = std::max(diagnosticMaxDecodeMs, elapsedMs);
        const auto now = std::chrono::steady_clock::now();
        if (diagnosticLast != std::chrono::steady_clock::time_point{}
            && now - diagnosticLast < std::chrono::seconds(1)) return;
        LOG_INFO("[PlaybackDiag] session gen={} identity='{}' path='{}' request={} source_s={:.6f} "
                 "audio=[{},{}) calls={} reopens={} lost_picture={} lost_audio={} reads={} video_frames={} audio_frames={} "
                 "decode_avg_ms={:.3f} decode_max_ms={:.3f} open_ms={:.3f} cache_images={} cache_sound={} "
                 "pending_video={} pending_bytes={} decoded_video_s={:.6f} selected_s={:.6f} "
                 "lookahead_s={:.6f} coverage_lost={} audio_end={} discarded_audio_before={} eof={} video_drained={} "
                 "hardware_requested={} has_video={} has_audio={} status={} last_reopen_source_s={:.6f} "
                 "last_reopen_selected_s={:.6f} last_reopen_decoded_video_s={:.6f} "
                 "last_reopen_audio_first={} last_reopen_discarded_audio_before={}",
                 context.generation, diagnosticIdentity, sourcePath, seconds ? "video+audio" : "audio",
                 seconds.value_or(-1.0), first, last, diagnosticCalls, diagnosticReopens,
                 diagnosticLostPictures, diagnosticLostAudio, diagnosticReads, diagnosticVideoFrames,
                 diagnosticAudioFrames, diagnosticDecodeMs / diagnosticCalls, diagnosticMaxDecodeMs, diagnosticOpenMs,
                 images.size(), sound.size(), pendingVideo.size(), pendingVideoBytes, decodedVideoSeconds,
                 selectedSeconds, lookaheadVideo ? lookaheadVideo->first : -1.0, videoCoverageLost,
                 audioEnd, discardedAudioBefore, eof, videoDrained,
                 hardwareDecode, video != nullptr, audio != nullptr, static_cast<int>(status),
                 diagnosticRestartSource, diagnosticRestartSelected, diagnosticRestartDecodedVideo,
                 diagnosticRestartFirst, diagnosticRestartDiscardedAudio);
        Logger::ModuleLogger(__FILE__)->flush();
        diagnosticLast = now;
        diagnosticCalls = diagnosticReopens = diagnosticLostPictures = diagnosticLostAudio = 0;
        diagnosticReads = diagnosticVideoFrames = diagnosticAudioFrames = 0;
        diagnosticDecodeMs = diagnosticMaxDecodeMs = diagnosticOpenMs = 0.0;
    }

    Status restart(const Context& context) {
        if (!seekable) {
            error = "Cannot seek backward in non-seekable media";
            return Status::Failure;
        }
        const auto path = sourcePath;
        const auto spec = outputSpec;
        return open(path, hardwareDecode, spec, wantsVideo, context);
    }

    void appendSound(std::shared_ptr<AudioFrame> converted) {
        if (!converted) return;
        if (!converted->hasPosition()) converted->setPosition(inferredAudioPosition);
        inferredAudioPosition = converted->position() + converted->samples();
        audioEnd = std::max(audioEnd, inferredAudioPosition);
        sound.push_back(std::move(converted));
    }

    void pruneAudio(int64_t before) {
        sound.erase(std::remove_if(sound.begin(), sound.end(), [&](const auto& frame) {
            const auto end = frame->position() + frame->samples();
            if (end > before) return false;
            discardedAudioBefore = std::max(discardedAudioBefore, end);
            return true;
        }), sound.end());
    }

    bool pictureReady(double seconds) const {
        return !video || videoDrained || (videoEnd >= 0 && seconds >= videoEnd)
            || (!images.empty() && lookaheadVideo && lookaheadVideo->first > seconds + 0.0000001)
            || (!images.empty() && images.front().first <= seconds + 0.0000001
                && decodedVideoSeconds >= seconds - 0.0000001);
    }

    bool soundReady(int64_t last) const {
        return !audio || audioEnd > last || eof;
    }

    Status pullVideo(const Context& context, std::optional<double> seconds) {
        if (!video) return Status::Succeeded;
        if (seconds && lookaheadVideo && lookaheadVideo->first <= *seconds + 0.0000001) {
            images.clear();
            images.emplace_back(std::move(*lookaheadVideo));
            lookaheadVideo.reset();
        }
        for (;;) {
            if (cancelled(context)) return Status::Cancelled;
            if (seconds && pictureReady(*seconds)) break;
            auto item = video->pull(context.generation);
            if (video->lastError() < 0) {
                error = "Video decode failed: " + ffmpegError(video->lastError());
                return Status::Failure;
            }
            if (item.type == MediaFrameType::Eof) videoDrained = true;

            auto image = item.videoFrame();
            if (!image) {
                if (!pendingVideo.empty()) {
                    auto packet = std::move(pendingVideo.front());
                    pendingVideo.pop_front();
                    pendingVideoBytes -= std::get<PacketPtr>(packet.payload)->size();
                    if (!video->push(packet)) {
                        error = "Decoder rejected video packet: " + ffmpegError(video->lastError());
                        return Status::Failure;
                    }
                    continue;
                }
                if (eof && !videoDrainSent) {
                    videoDrainSent = true;
                    if (!video->push(MediaFrame::eof(context.generation))) {
                        error = "Video decoder drain failed";
                        return Status::Failure;
                    }
                    continue;
                }
                break;
            }
            ++diagnosticVideoFrames;

            const auto native = image->native();
            const double position = hasFrameTimestamp(*native) ? frameTimeSeconds(*native) : inferredVideoSeconds;
            const double span = native->duration > 0 ? native->duration * av_q2d(native->time_base) : 1.0 / rate.toDouble();
            inferredVideoSeconds = position + span;
            lastImageEnd = inferredVideoSeconds;
            decodedVideoSeconds = position;
            if (!seconds || position > *seconds + 0.0000001) {
                if (seconds) {
                    lookaheadVideo = std::make_pair(position, std::move(image));
                    break;
                }
                continue;
            }
            images.clear();
            images.emplace_back(position, std::move(image));
        }
        return Status::Succeeded;
    }

    Status pullAudio(const Context& context, int64_t before) {
        if (audio) for (;;) {
            if (cancelled(context)) return Status::Cancelled;
            auto item = audio->pull(context.generation);
            if (audio->lastError() < 0) {
                error = "Audio decode failed: " + ffmpegError(audio->lastError());
                return Status::Failure;
            }
            auto frame = item.audioFrame();
            if (!frame) break;

            ++diagnosticAudioFrames;
            if (frame->hasPosition() && inputPositionKnown
                && std::abs(static_cast<long double>(frame->position()) - expectedInputPosition) > 2) {
                while (auto tail = resampler.drain()) appendSound(std::move(tail));
                resampler.reset();
            }
            if (frame->hasPosition()) {
                expectedInputPosition = frame->position() + frame->samples();
                inputPositionKnown = true;
            } else if (inputPositionKnown) {
                expectedInputPosition += frame->samples();
            }
            appendSound(resampler.convert(*frame, outputSpec));
            pruneAudio(before);
            if (sound.size() > kAudioCapacity) {
                error = "Audio decoder lookahead exceeded bounded cache";
                return Status::Failure;
            }
        }
        if (eof && !resamplerDrained) {
            while (auto tail = resampler.drain()) appendSound(std::move(tail));
            resamplerDrained = true;
            pruneAudio(before);
        }
        return Status::Succeeded;
    }

    Status read(const Context& context) {
        ++diagnosticReads;
        auto result = demux->readResult(context.generation);
        if (result.status == demuxer::ReadStatus::Cancelled) return Status::Cancelled;
        if (result.status == demuxer::ReadStatus::Failure) {
            error = "Media read failed: " + ffmpegError(result.error);
            return Status::Failure;
        }
        if (result.status == demuxer::ReadStatus::End) {
            if (audio && !audio->push(MediaFrame::eof(context.generation))) {
                error = "Audio decoder drain failed";
                return Status::Failure;
            }
            eof = true;
        } else if (video && result.frame.metadata.streamIndex == videoStream->index) {
            pendingVideoBytes += std::get<PacketPtr>(result.frame.payload)->size();
            pendingVideo.push_back(std::move(result.frame));
            if (pendingVideo.size() > kVideoPacketCapacity || pendingVideoBytes > kVideoPacketBytes) {
                error = "Video packet lookahead exceeded bounded cache";
                return Status::Failure;
            }
        } else if (audio && result.frame.metadata.streamIndex == audioStream->index && !audio->push(result.frame)) {
            error = "Decoder rejected audio packet: " + ffmpegError(audio->lastError());
            return Status::Failure;
        }
        return Status::Succeeded;
    }

    Status decode(std::optional<double> seconds, int64_t first, int64_t last, const Context& context,
                  std::shared_ptr<VideoFrame>* result = nullptr) {
        const auto diagnosticStart = std::chrono::steady_clock::now();
        auto finish = [&](Status status) {
            reportDecode(std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - diagnosticStart).count(), seconds, first, last, context, status);
            return status;
        };
        const bool lostPicture = seconds && video && (videoCoverageLost
            || (!images.empty() && *seconds + 0.0000001 < images.front().first));
        if (lostPicture || (audio && first < discardedAudioBefore)) {
            ++diagnosticReopens;
            diagnosticRestartSource = seconds.value_or(-1.0);
            diagnosticRestartSelected = selectedSeconds;
            diagnosticRestartDecodedVideo = decodedVideoSeconds;
            diagnosticRestartFirst = first;
            diagnosticRestartDiscardedAudio = discardedAudioBefore;
            diagnosticLostPictures += lostPicture;
            diagnosticLostAudio += audio && first < discardedAudioBefore;

            auto status = restart(context);
            if (status != Status::Succeeded) return finish(status);
        }

        if (!seconds && soundReady(last)) return finish(Status::Succeeded);
        if (!seconds) {
            images.clear();
            lookaheadVideo.reset();
            videoCoverageLost = video != nullptr;
        }

        InterruptScope interrupt(*demux, context);
        for (;;) {
            if (cancelled(context)) return finish(Status::Cancelled);
            auto status = pullAudio(context, first);
            if (status != Status::Succeeded) return finish(status);
            status = pullVideo(context, seconds);
            if (status != Status::Succeeded) return finish(status);
            if ((!seconds || pictureReady(*seconds)) && soundReady(last)) break;
            status = read(context);
            if (status != Status::Succeeded) return finish(status);
        }
        if (seconds) {
            selectedSeconds = *seconds;
            if (result && *result) videoCoverageLost = false;
            if (result && !images.empty() && images.front().first <= *seconds + 0.0000001
                && !(videoEnd >= 0 && *seconds >= videoEnd)
                && !(videoDrained && lastImageEnd >= 0 && *seconds >= lastImageEnd))
                *result = images.front().second;
            if (result && !*result) images.clear();
        }
        return finish(Status::Succeeded);
    }
    float sample(int channel, int64_t position) const {
        for (const auto& frame : sound) {
            if (position < frame->position()) continue;
            const uint64_t offset = static_cast<uint64_t>(position) - static_cast<uint64_t>(frame->position());
            if (offset < static_cast<uint64_t>(frame->samples())) {
                return frame->sampleRo(channel, static_cast<int>(offset));
            }
        }
        return 0.0f;
    }
};
}

struct CompositionExecutor::Impl {
    std::shared_ptr<const CompositionPlan> snapshot;
    Options options;
    Info info;
    std::vector<std::string> warnings;
    std::unordered_map<std::string, std::unique_ptr<DecoderSession>> sessions;
    std::unordered_map<std::string, std::string> failures;
    std::optional<uint64_t> generation;
    std::optional<int64_t> audioCursor;
    void reset() {
        sessions.clear();
        failures.clear();
        audioCursor.reset();
        generation.reset();
    }

    void prepare(const Context& context) {
        if (generation && *generation != context.generation) reset();
        generation = context.generation;
    }

    std::string identity(const char* lane, size_t index) const {
        return std::string(lane) + std::to_string(index);
    }

    Status failure(const std::string& key, const std::string& path, const std::string& message, std::string& error) {
        error = message;
        failures[key] = error;
        sessions.erase(key);
        if (options.errorPolicy == CompositionErrorPolicy::Strict) return Status::Failure;
        const std::string warning = "unavailable media: " + path + " (" + error + ")";
        if (std::find(warnings.begin(), warnings.end(), warning) == warnings.end()) warnings.push_back(warning);
        return Status::Succeeded;
    }

    Status session(const std::string& key, const CompositionClip& clip, bool wantVideo,
                   const Context& context, DecoderSession*& result, std::string& error) {
        result = nullptr;
        if (auto found = sessions.find(key); found != sessions.end()) {
            result = found->second.get();
            return Status::Succeeded;
        }
        if (auto failed = failures.find(key); failed != failures.end()) {
            error = failed->second;
            return options.errorPolicy == CompositionErrorPolicy::Strict ? Status::Failure : Status::Succeeded;
        }
        if (clip.sourcePath.empty()) {
            if (options.errorPolicy == CompositionErrorPolicy::Preview) return Status::Succeeded;
            return failure(key, clip.mediaRef, "Missing media source: " + clip.mediaRef, error);
        }
        auto created = std::make_unique<DecoderSession>();
        created->diagnosticIdentity = key;
        auto status = created->open(clip.sourcePath, options.hardwareDecode, info.settings.audioSpec(), wantVideo, context);
        if (status == Status::Cancelled) return status;
        if (status == Status::Failure) return failure(key, clip.sourcePath, created->error, error);
        result = created.get();
        sessions.emplace(key, std::move(created));
        return Status::Succeeded;
    }

    void retain(const std::string& lane, const std::vector<std::string>& keys) {
        auto unused = [&](const std::string& key) {
            return key.compare(0, lane.size(), lane) == 0 && std::find(keys.begin(), keys.end(), key) == keys.end();
        };
        for (auto it = sessions.begin(); it != sessions.end();) {
            if (unused(it->first)) it = sessions.erase(it);
            else ++it;
        }
        for (auto it = failures.begin(); it != failures.end();) {
            if (unused(it->first)) it = failures.erase(it);
            else ++it;
        }
    }

    const CompositionClip* clipAt(const std::vector<CompositionClip>& clips, int64_t sample, size_t& index) const {
        auto next = std::upper_bound(clips.begin(), clips.end(), sample,
            [&](int64_t p, const CompositionClip& clip) { return p < info.settings.audioSamplePosition(clip.startFrame); });
        if (next == clips.begin()) return nullptr;
        const auto found = std::prev(next);
        if (sample >= info.settings.audioSamplePosition(found->startFrame + found->durationFrames)) return nullptr;
        index = static_cast<size_t>(found - clips.begin());
        return &*found;
    }

    int64_t boundary(const std::vector<CompositionClip>& clips, int64_t begin, int64_t end) const {
        for (const auto& clip : clips) {
            const auto start = info.settings.audioSamplePosition(clip.startFrame);
            const auto stop = info.settings.audioSamplePosition(clip.startFrame + clip.durationFrames);
            if (start > begin) end = std::min(end, start);
            if (stop > begin) end = std::min(end, stop);
        }
        return end;
    }

    double sourcePosition(const CompositionClip& clip, int64_t target) const {
        const long double rate = info.settings.sampleRate();
        const long double timeBase = static_cast<long double>(info.settings.timeBase().numerator())
            / info.settings.timeBase().denominator();
        const long double local = static_cast<long double>(target)
            - info.settings.audioSamplePosition(clip.startFrame);
        return static_cast<double>(clip.trimStartFrame * timeBase * rate + local * clip.speed);
    }

    Status mixContribution(const CompositionClip& clip, size_t index, const char* lane, bool muted,
                           int64_t begin, int64_t end, AudioFrame& output, int64_t outputBegin,
                           const Context& context, std::vector<std::string>& used, std::string& error) {
        const auto key = identity(lane, index);
        used.push_back(key);
        DecoderSession* decoder = nullptr;
        auto status = session(key, clip, std::string(lane) == "video:", context, decoder, error);
        if (status != Status::Succeeded || !decoder || !decoder->audio) return status;
        const double firstPosition = sourcePosition(clip, begin);
        const double lastPosition = sourcePosition(clip, end - 1);
        if (!std::isfinite(firstPosition) || !std::isfinite(lastPosition)
            || firstPosition < static_cast<double>(std::numeric_limits<int64_t>::min())
            || lastPosition >= static_cast<double>(std::numeric_limits<int64_t>::max() - 2)) {
            error = "Audio source sample mapping overflow";
            return Status::Failure;
        }
        const auto first = static_cast<int64_t>(std::floor(firstPosition));
        const auto last = static_cast<int64_t>(std::floor(lastPosition)) + 1;
        status = decoder->decode(std::nullopt, first, last, context);
        if (status == Status::Cancelled) return status;
        if (status == Status::Failure) return failure(key, clip.sourcePath, decoder->error, error);
        if (muted) return Status::Succeeded;
        const int rate = info.settings.sampleRate();
        const auto clipStart = info.settings.audioSamplePosition(clip.startFrame);
        const double duration = info.settings.secondsFromFrame(clip.durationFrames);
        const double fadeIn = info.settings.secondsFromFrame(clip.fadeInFrames);
        const double fadeOut = info.settings.secondsFromFrame(clip.fadeOutFrames);
        for (int64_t target = begin; target < end; ++target) {
            if ((target - begin) % 256 == 0 && cancelled(context)) return Status::Cancelled;
            const double time = static_cast<double>(target - clipStart) / rate;
            double gain = clip.volume;
            if (fadeIn > 0) gain *= std::clamp(time / fadeIn, 0.0, 1.0);
            if (fadeOut > 0) gain *= std::clamp((duration - time) / fadeOut, 0.0, 1.0);
            const double position = sourcePosition(clip, target);
            const auto lower = static_cast<int64_t>(std::floor(position));
            const double fraction = position - lower;
            for (int channel = 0; channel < info.settings.channels(); ++channel) {
                const double value = decoder->sample(channel, lower) * (1.0 - fraction)
                    + decoder->sample(channel, lower + 1) * fraction;
                output.sampleRw(channel, static_cast<int>(target - outputBegin)) += static_cast<float>(value * gain);
            }
        }
        decoder->pruneAudio(static_cast<int64_t>(std::floor(sourcePosition(clip, end))));
        return Status::Succeeded;
    }
};

CompositionExecutor::CompositionExecutor() : impl_(std::make_unique<Impl>()) {}
CompositionExecutor::~CompositionExecutor() = default;
void CompositionExecutor::reset() {
    impl_->reset();
}
void CompositionExecutor::close() {
    impl_->reset();
    impl_->snapshot.reset();
    impl_->info = {};
    impl_->warnings.clear();
}

const CompositionExecutor::Info& CompositionExecutor::info() const {
    return impl_->info;
}
const CompositionPlan& CompositionExecutor::plan() const {
    static const CompositionPlan empty;
    return impl_->snapshot ? *impl_->snapshot : empty;
}
const std::vector<std::string>& CompositionExecutor::warnings() const {
    return impl_->warnings;
}

CompositionExecutor::OpenResult CompositionExecutor::open(const CompositionPlan& plan, Options options, Context context) {
    auto snapshot = std::make_shared<const CompositionPlan>(plan);
    close();
    if (cancelled(context)) return {Status::Cancelled, {}, {}};
    const auto& owned = *snapshot;
    std::string error;
    if (!owned.settings.valid(&error) || owned.frameCount < 0)
        return {Status::Failure, {}, error.empty() ? "Negative composition duration" : error};
    auto validate = [&](const std::vector<CompositionClip>& clips) {
        int64_t previousEnd = 0;
        for (const auto& clip : clips) {
            if (clip.startFrame < previousEnd || clip.durationFrames < 0 || clip.trimStartFrame < 0
                || clip.startFrame > owned.frameCount || clip.durationFrames > owned.frameCount - clip.startFrame
                || !std::isfinite(clip.speed) || clip.speed <= 0 || !std::isfinite(clip.volume)
                || clip.fadeInFrames < 0 || clip.fadeOutFrames < 0) return false;
            previousEnd = clip.startFrame + clip.durationFrames;
        }
        return true;
    };
    if (!validate(owned.clips) || !validate(owned.audioClips)) return {Status::Failure, {}, "Invalid or overlapping composition clips"};
    impl_->snapshot = std::move(snapshot);
    impl_->options = options;
    impl_->warnings = impl_->snapshot->warnings;
    impl_->info.settings = impl_->snapshot->settings;
    impl_->info.frameCount = impl_->snapshot->frameCount;
    impl_->info.audioSampleCount = impl_->info.settings.audioSamplePosition(impl_->info.frameCount);
    impl_->info.duration = impl_->info.settings.secondsFromFrame(impl_->info.frameCount);
    impl_->info.seekable = impl_->info.frameCount > 0;
    impl_->info.hasVideo = !impl_->snapshot->clips.empty();
    impl_->info.hasAudio = !impl_->snapshot->clips.empty() || !impl_->snapshot->audioClips.empty();
    impl_->prepare(context);
    return {Status::Succeeded, impl_->info, {}};
}

CompositionExecutor::OpenResult CompositionExecutor::openSource(const std::string& path, Options options, Context context) {
    const std::string sourcePath = path;
    close();
    if (cancelled(context)) return {Status::Cancelled, {}, {}};
    auto decoder = std::make_unique<DecoderSession>();
    decoder->diagnosticIdentity = "video:0";
    auto settings = TimelineSettings::hd1080p24();
    const auto status = decoder->open(sourcePath, options.hardwareDecode, settings.audioSpec(), true, context);
    if (status != Status::Succeeded) return {status, {}, status == Status::Failure ? decoder->error : std::string{}};
    auto video = settings.videoSpec();
    if (decoder->videoStream) {
        video.width = decoder->videoStream->codec.width();
        video.height = decoder->videoStream->codec.height();
    }
    video.frameRate = decoder->rate;
    video.timeBase = decoder->rate.flipped();
    settings = TimelineSettings(video, settings.audioSpec());
    const double frames = std::ceil(decoder->duration * settings.fps());
    const double samples = std::round(decoder->duration * settings.sampleRate());
    if (!std::isfinite(frames) || frames <= 0 || frames >= static_cast<double>(std::numeric_limits<int64_t>::max())
        || !std::isfinite(samples) || samples >= static_cast<double>(std::numeric_limits<int64_t>::max())) {
        return {Status::Failure, {}, "Media duration is empty or exceeds sample/frame range"};
    }

    CompositionPlan sourcePlan;
    sourcePlan.settings = settings;
    sourcePlan.frameCount = static_cast<int64_t>(frames);
    CompositionClip clip;
    clip.sourcePath = sourcePath;
    clip.durationFrames = sourcePlan.frameCount;
    sourcePlan.clips = {clip};
    impl_->snapshot = std::make_shared<const CompositionPlan>(std::move(sourcePlan));
    impl_->options = options;
    impl_->info.settings = settings;
    impl_->info.frameCount = static_cast<int64_t>(frames);
    impl_->info.audioSampleCount = static_cast<int64_t>(samples);
    impl_->info.duration = decoder->duration;
    impl_->info.sourceMode = true;
    impl_->info.seekable = decoder->seekable;
    impl_->info.hasVideo = decoder->video != nullptr;
    impl_->info.hasAudio = decoder->audio != nullptr;
    impl_->sessions.emplace("video:0", std::move(decoder));
    impl_->prepare(context);
    return {Status::Succeeded, impl_->info, {}};
}

CompositionExecutor::VideoResult CompositionExecutor::videoFrame(int64_t frame, Context context) {
    if (cancelled(context)) return {Status::Cancelled, {}, {}};
    if (!impl_->snapshot) return {Status::Failure, {}, "Composition is not open"};
    if (frame < 0) return {Status::Failure, {}, "Negative video frame"};
    if (frame >= impl_->info.frameCount) return {Status::End, {}, {}};
    impl_->prepare(context);
    CompositionVideoSample output;
    output.settings = impl_->info.settings;
    output.targetPosition = frame;
    output.targetTimeBase = output.settings.timeBase();
    output.generation = context.generation;
    output.sourceMode = impl_->info.sourceMode;
    output.plan = impl_->snapshot;
    std::vector<std::string> used;
    std::string error;
    if (auto sample = impl_->snapshot->videoSampleAt(frame)) {
        const auto& clip = impl_->snapshot->clips[sample->clipIndex];
        output.clipId = clip.id;
        output.mediaRef = clip.mediaRef;
        output.clipLocalFrame = sample->clipLocalFrame;
        const auto key = impl_->identity("video:", sample->clipIndex);
        used.push_back(key);
        DecoderSession* decoder = nullptr;
        auto status = impl_->session(key, clip, true, context, decoder, error);
        if (status == Status::Succeeded && decoder) {
            const auto begin = impl_->info.settings.audioSamplePosition(frame);
            const auto end = std::min(impl_->info.audioSampleCount, impl_->info.settings.audioSamplePosition(frame + 1));
            const double firstPosition = impl_->sourcePosition(clip, begin);
            const double lastPosition = impl_->sourcePosition(clip, std::max(begin, end - 1));
            if (!std::isfinite(firstPosition) || !std::isfinite(lastPosition)
                || firstPosition < static_cast<double>(std::numeric_limits<int64_t>::min())
                || lastPosition >= static_cast<double>(std::numeric_limits<int64_t>::max() - 2)) {
                return {Status::Failure, {}, "Audio source sample mapping overflow"};
            }
            const auto first = static_cast<int64_t>(std::floor(firstPosition));
            const auto last = static_cast<int64_t>(std::floor(lastPosition)) + 1;
            const auto prefetchLast = static_cast<int64_t>(std::min(
                static_cast<long double>(last), static_cast<long double>(first) + kMixBlock));
            status = decoder->decode(sample->hidden ? std::nullopt : std::optional<double>(sample->sourceSeconds),
                first, prefetchLast, context, &output.source);
            if (status == Status::Failure) status = impl_->failure(key, clip.sourcePath, decoder->error, error);
        }
        if (status == Status::Cancelled) {
            reset();
            return {status, {}, {}};
        }
        if (status == Status::Failure) return {status, {}, error};
        if (sample->hidden) output.source.reset();
    }
    impl_->retain("video:", used);
    if (cancelled(context)) {
        reset();
        return {Status::Cancelled, {}, {}};
    }
    output.blank = !output.source;
    return {Status::Succeeded, std::move(output), {}};
}

CompositionExecutor::AudioResult CompositionExecutor::audioSamples(int64_t begin, int64_t end, Context context) {
    if (cancelled(context)) return {Status::Cancelled, {}, {}};
    if (!impl_->snapshot) return {Status::Failure, {}, "Composition is not open"};
    if (begin < 0 || end < begin || end - begin > kMaxAudioRequest)
        return {Status::Failure, {}, "Invalid audio range (maximum 1048576 samples per call)"};
    if (begin >= impl_->info.audioSampleCount) return {Status::End, {}, {}};
    impl_->prepare(context);
    if (impl_->audioCursor && begin < *impl_->audioCursor) {
        if (impl_->info.sourceMode && !impl_->info.seekable)
            return {Status::Failure, {}, "Cannot seek backward in non-seekable audio"};
        impl_->retain("independent-audio:", {});
    }
    end = std::min(end, impl_->info.audioSampleCount);
    auto output = std::make_shared<AudioFrame>();
    output->setSpec(impl_->info.settings.audioSpec());
    output->setSamples(static_cast<int>(end - begin), true);
    output->setPosition(begin);
    output->clear();
    const auto& plan = *impl_->snapshot;
    std::string error;
    for (int64_t cursor = begin; cursor < end;) {
        if (cancelled(context)) {
            reset();
            return {Status::Cancelled, {}, {}};
        }

        int64_t stop = std::min(end, cursor + std::min(kMixBlock, end - cursor));
        stop = impl_->boundary(plan.clips, cursor, stop);
        stop = impl_->boundary(plan.audioClips, cursor, stop);
        size_t videoIndex = 0, audioIndex = 0;
        const auto* videoClip = impl_->clipAt(plan.clips, cursor, videoIndex);
        const auto* audioClip = impl_->clipAt(plan.audioClips, cursor, audioIndex);
        for (const auto* clip : {videoClip, audioClip}) {
            if (clip && clip->speed > 1.0) {
                stop = std::min(stop, cursor + static_cast<int64_t>(std::max(1.0, std::floor(kMixBlock / clip->speed))));
            }
        }

        std::vector<std::string> original, independent;
        Status status = Status::Succeeded;
        if (videoClip)
            status = impl_->mixContribution(*videoClip, videoIndex, "video:", plan.muted,
                cursor, stop, *output, begin, context, original, error);
        if (status == Status::Succeeded && audioClip)
            status = impl_->mixContribution(*audioClip, audioIndex, "independent-audio:", plan.audioMuted,
                cursor, stop, *output, begin, context, independent, error);
        impl_->retain("video:", original);
        impl_->retain("independent-audio:", independent);
        if (status == Status::Cancelled) {
            reset();
            return {status, {}, {}};
        }
        if (status == Status::Failure) return {status, {}, error};
        cursor = stop;
    }
    for (int channel = 0; channel < output->spec().channels; ++channel)
        for (int sample = 0; sample < output->samples(); ++sample)
            output->sampleRw(channel, sample) = std::clamp(output->sampleRo(channel, sample), -1.0f, 1.0f);
    if (cancelled(context)) {
        reset();
        return {Status::Cancelled, {}, {}};
    }
    impl_->audioCursor = end;
    return {Status::Succeeded, std::move(output), {}};
}
} // namespace heisenberg
