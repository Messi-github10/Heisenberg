#include "Producer.hpp"

#include "ProfileNormalizer.hpp"

#include <Common/FrameTime.hpp>
#include <Common/Packet.hpp>
#include <Common/Stream.hpp>
#include <Platform/D3D11/D3D11Context.hpp>
#include <Video/Pipeline/DecoderNode.hpp>
#include <Video/Pipeline/DemuxSource.hpp>
#include <Utiles/Logger.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
#include <libavutil/rational.h>
#include <libswresample/swresample.h>
}

namespace heisenberg {
namespace {

constexpr double kSeekPrerollSeconds = 2.0;
constexpr int64_t kForwardDecodeThresholdFrames = 64;

void setError(std::string* error, const std::string& message) {
    if (error) *error = message;
}

std::string ffmpegError(int code) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
    av_strerror(code, buffer.data(), buffer.size());
    return buffer.data();
}

AVRational sourceFrameRate(const Stream& stream) {
    if (stream.codec.fpsNum > 0 && stream.codec.fpsDen > 0) {
        return {stream.codec.fpsNum, stream.codec.fpsDen};
    }
    return {25, 1};
}

int64_t sourceIndexFromSeconds(double seconds, AVRational frameRate) {
    const double fps = av_q2d(frameRate);
    if (fps <= 0.0) return 0;
    return std::max<int64_t>(0, static_cast<int64_t>(std::llround(seconds * fps)));
}

std::shared_ptr<ProfileNormalizer> acquireNormalizer(const Profile& profile,
                                                     std::string* error) {
    static std::mutex mutex;
    static std::shared_ptr<ProfileNormalizer> instance;
    static Profile cached;
    std::lock_guard<std::mutex> lock(mutex);
    if (instance && instance->isReady() && cached == profile) return instance;

    auto created = std::make_shared<ProfileNormalizer>();
    if (!created->initialize(profile, error)) return {};
    cached = profile;
    instance = std::move(created);
    return instance;
}

class AudioResampler {
public:
    ~AudioResampler() {
        swr_free(&context_);
    }

    bool convert(const AudioFrame& input,
                 int outputSampleRate,
                 int outputChannels,
                 std::vector<std::vector<float>>& fifo) {
        if (!input.spec().valid() || input.samples() <= 0) return false;
        if (!configure(input.spec(), outputSampleRate, outputChannels)) return false;

        const int maxSamples = static_cast<int>(av_rescale_rnd(
            swr_get_delay(context_, input.spec().sampleRate) + input.samples(),
            outputSampleRate, input.spec().sampleRate, AV_ROUND_UP));
        if (maxSamples <= 0) return true;

        std::vector<std::vector<float>> converted(
            static_cast<size_t>(outputChannels),
            std::vector<float>(static_cast<size_t>(maxSamples)));
        std::vector<uint8_t*> outPlanes(static_cast<size_t>(outputChannels));
        std::vector<const uint8_t*> inPlanes(static_cast<size_t>(input.spec().channels));
        for (int channel = 0; channel < outputChannels; ++channel) {
            outPlanes[channel] = reinterpret_cast<uint8_t*>(converted[channel].data());
        }
        for (int channel = 0; channel < input.spec().channels; ++channel) {
            inPlanes[channel] = reinterpret_cast<const uint8_t*>(input.channelRo(channel));
        }

        const int samples = swr_convert(
            context_, outPlanes.data(), maxSamples,
            inPlanes.data(), input.samples());
        if (samples < 0) return false;
        if (samples == 0) return true;

        if (fifo.size() != static_cast<size_t>(outputChannels)) {
            fifo.assign(static_cast<size_t>(outputChannels), {});
        }
        for (int channel = 0; channel < outputChannels; ++channel) {
            fifo[channel].insert(fifo[channel].end(),
                                 converted[channel].begin(),
                                 converted[channel].begin() + samples);
        }
        return true;
    }

    void reset() {
        swr_free(&context_);
        inputRate_ = 0;
        inputChannels_ = 0;
        outputRate_ = 0;
        outputChannels_ = 0;
    }

private:
    bool configure(const AudioSpec& input, int outputSampleRate, int outputChannels) {
        if (context_ &&
            inputRate_ == input.sampleRate &&
            inputChannels_ == input.channels &&
            outputRate_ == outputSampleRate &&
            outputChannels_ == outputChannels) {
            return true;
        }

        swr_free(&context_);
        inputRate_ = input.sampleRate;
        inputChannels_ = input.channels;
        outputRate_ = outputSampleRate;
        outputChannels_ = outputChannels;
        if (inputRate_ <= 0 || inputChannels_ <= 0 ||
            outputRate_ <= 0 || outputChannels_ <= 0) {
            return false;
        }

        AVChannelLayout inLayout{};
        AVChannelLayout outLayout{};
        av_channel_layout_default(&inLayout, inputChannels_);
        av_channel_layout_default(&outLayout, outputChannels_);
        const int result = swr_alloc_set_opts2(
            &context_,
            &outLayout, AV_SAMPLE_FMT_FLTP, outputRate_,
            &inLayout, AV_SAMPLE_FMT_FLTP, inputRate_,
            0, nullptr);
        av_channel_layout_uninit(&inLayout);
        av_channel_layout_uninit(&outLayout);
        if (result < 0 || !context_ || swr_init(context_) < 0) {
            swr_free(&context_);
            return false;
        }
        return true;
    }

    SwrContext* context_ = nullptr;
    int inputRate_ = 0;
    int inputChannels_ = 0;
    int outputRate_ = 0;
    int outputChannels_ = 0;
};

} // namespace

struct Producer::Impl {
    Profile profile = Profile::hd1080p24();
    decoder::DecoderConfig decoderConfig{
        decoder::DecoderBackend::Software, false};
    std::string resource;
    std::unique_ptr<pipeline::DemuxSource> demux;
    std::unique_ptr<pipeline::DecoderNode> videoDecoder;
    std::unique_ptr<pipeline::DecoderNode> audioDecoder;
    const Stream* videoStream = nullptr;
    const Stream* audioStream = nullptr;
    AVRational videoRate{24, 1};
    double durationSeconds = 0.0;
    int64_t length = 0;
    int64_t position = 0;
    bool seekableMedia = false;
    bool eof = false;
    uint64_t generation = 0;
    int64_t lastSourceIndex = -1;
    std::shared_ptr<AVFrame> lastVideo;
    std::shared_ptr<AVFrame> lastNormalized;
    std::shared_ptr<ProfileNormalizer> normalizer;
    AudioResampler resampler;
    int64_t audioStart = 0;
    std::vector<std::vector<float>> audioFifo;

    void resetPipeline() {
        if (videoDecoder) videoDecoder->close();
        if (audioDecoder) audioDecoder->close();
        if (demux) demux->close();
        videoDecoder.reset();
        audioDecoder.reset();
        demux.reset();
        videoStream = nullptr;
        audioStream = nullptr;
        durationSeconds = 0.0;
        length = 0;
        position = 0;
        seekableMedia = false;
        eof = false;
        lastSourceIndex = -1;
        lastVideo.reset();
        lastNormalized.reset();
        normalizer.reset();
        resampler.reset();
        audioStart = 0;
        audioFifo.clear();
    }

    int64_t clampPosition(int64_t value) const {
        if (length <= 0) return 0;
        return std::clamp(value, int64_t{0}, length - 1);
    }

    int64_t sourceIndexFor(int64_t profilePosition) const {
        return sourceIndexFromSeconds(
            profile.secondsFromFrame(profilePosition), videoRate);
    }

    void dropAudioBefore(int64_t sample) {
        if (audioFifo.empty()) {
            audioStart = sample;
            return;
        }
        if (sample <= audioStart) return;
        const int64_t available = static_cast<int64_t>(audioFifo[0].size());
        const int64_t drop = std::min(sample - audioStart, available);
        for (auto& channel : audioFifo) {
            channel.erase(channel.begin(),
                          channel.begin() + static_cast<std::ptrdiff_t>(drop));
        }
        audioStart += drop;
        if (audioFifo[0].empty()) audioFifo.clear();
    }

    void appendAudio(const AudioFramePtr& frame) {
        if (!frame) return;
        if (!resampler.convert(*frame, profile.sampleRate(), profile.channels(),
                               audioFifo)) {
            LOG_WARN("Producer: audio resample failed");
        }
    }

    AudioFramePtr takeAudio(int64_t profilePosition) {
        const int64_t start = profile.audioSamplePosition(profilePosition);
        const int samples = static_cast<int>(
            profile.audioSamplesForFrame(profilePosition));
        auto output = std::make_shared<AudioFrame>();
        AudioSpec spec;
        spec.sampleRate = profile.sampleRate();
        spec.channels = profile.channels();
        spec.layout = defaultLayout(spec.channels);
        output->setSpec(spec);
        output->setSamples(samples, true);
        output->clear();
        output->setPosition(start);
        if (samples <= 0) return output;

        dropAudioBefore(start);
        if (audioFifo.empty()) return output;

        const int64_t available = static_cast<int64_t>(audioFifo[0].size());
        const int64_t skip = std::max<int64_t>(0, start - audioStart);
        if (skip >= available) return output;

        const int copy = static_cast<int>(
            std::min<int64_t>(samples, available - skip));
        for (int channel = 0; channel < spec.channels; ++channel) {
            std::memcpy(output->channelRw(channel),
                        audioFifo[channel].data() + skip,
                        static_cast<size_t>(copy) * sizeof(float));
        }
        dropAudioBefore(start + samples);
        return output;
    }

    bool pullVideo() {
        if (!videoDecoder) return false;
        MediaFrame output = videoDecoder->pull(generation);
        auto frame = output.videoFrame();
        if (!frame) return false;

        lastVideo = std::move(frame);
        if (!hasFrameTimestamp(*lastVideo)) {
            if (lastSourceIndex < 0) {
                lastVideo->pts = 0;
                lastVideo->time_base = av_inv_q(videoRate);
                lastSourceIndex = 0;
            } else {
                lastSourceIndex += 1;
                lastVideo->pts = lastSourceIndex;
                lastVideo->time_base = av_inv_q(videoRate);
            }
        } else {
            lastSourceIndex = frameIndexFromTimestamp(*lastVideo, videoRate);
        }
        return true;
    }

    bool pullAudio() {
        if (!audioDecoder) return false;
        MediaFrame output = audioDecoder->pull(generation);
        const auto frame = output.audioFrame();
        if (!frame) return output.type == MediaFrameType::Eof;
        appendAudio(frame);
        return true;
    }

    bool feedPacket() {
        if (!demux) return false;
        MediaFrame input = demux->read(generation);
        if (input.type == MediaFrameType::Packet) {
            if (videoStream && input.metadata.streamIndex == videoStream->index) {
                videoDecoder->push(input);
            } else if (audioDecoder && audioStream &&
                       input.metadata.streamIndex == audioStream->index) {
                audioDecoder->push(input);
            }
            return true;
        }
        if (input.type == MediaFrameType::Eof) {
            videoDecoder->push(input);
            if (audioDecoder) audioDecoder->push(input);
            eof = true;
            return true;
        }
        eof = true;
        return false;
    }

    bool decodeUntil(int64_t targetSourceIndex) {
        if (lastSourceIndex >= targetSourceIndex && lastVideo) return true;

        while (lastSourceIndex < targetSourceIndex || !lastVideo) {
            if (pullVideo()) {
                if (lastSourceIndex >= targetSourceIndex) return true;
                continue;
            }
            pullAudio();
            if (eof) {
                while (pullVideo()) {
                    if (lastSourceIndex >= targetSourceIndex) return true;
                }
                while (pullAudio()) {}
                return static_cast<bool>(lastVideo);
            }
            if (!feedPacket()) return static_cast<bool>(lastVideo);
        }
        return static_cast<bool>(lastVideo);
    }

    bool jumpTo(int64_t profilePosition) {
        if (!demux || !videoDecoder || !videoStream) return false;

        const int64_t targetSourceIndex = sourceIndexFor(profilePosition);
        const int64_t forwardDistance = targetSourceIndex - lastSourceIndex;
        const bool decodeForward = lastSourceIndex >= 0 && lastVideo
            && forwardDistance >= 0
            && forwardDistance <= kForwardDecodeThresholdFrames;

        if (!decodeForward) {
            const double seekSeconds = std::max(
                0.0, profile.secondsFromFrame(profilePosition) - kSeekPrerollSeconds);
            if (demux->seek(seekSeconds, videoStream->index, 1) < 0) {
                LOG_ERROR("Producer: seek to {:.3f}s failed", seekSeconds);
                return false;
            }
            videoDecoder->flush();
            if (audioDecoder) audioDecoder->flush();
            resampler.reset();
            audioFifo.clear();
            audioStart = profile.audioSamplePosition(profilePosition);
            lastSourceIndex = -1;
            lastVideo.reset();
            eof = false;
            ++generation;
        }

        return decodeUntil(targetSourceIndex);
    }
};

Producer::Producer(Profile profile)
    : impl_(std::make_unique<Impl>()) {
    impl_->profile = std::move(profile);
}

Producer::~Producer() {
    close();
}

void Producer::setHardwareDecode(bool enabled) {
    impl_->decoderConfig.preferred = enabled
        ? decoder::DecoderBackend::D3D11
        : decoder::DecoderBackend::Software;
    impl_->decoderConfig.allowFallback = enabled;
}

bool Producer::open(const std::string& path, std::string* error) {
    close();
    impl_->resource = path;
    impl_->demux = std::make_unique<pipeline::DemuxSource>();
    const int openResult = impl_->demux->open(path);
    if (openResult < 0) {
        setError(error, "Failed to open file: " + path + " (" +
                            ffmpegError(openResult) + ")");
        impl_->resetPipeline();
        return false;
    }

    impl_->durationSeconds = std::max(0.0, impl_->demux->duration());
    impl_->seekableMedia = impl_->demux->seekable();
    for (const Stream& stream : impl_->demux->streams()) {
        if (stream.isVideo() && !impl_->videoStream) {
            impl_->videoStream = &stream;
        } else if (stream.isAudio() && !impl_->audioStream) {
            impl_->audioStream = &stream;
        }
    }
    if (!impl_->videoStream) {
        setError(error, "No video stream found");
        impl_->resetPipeline();
        return false;
    }

    impl_->videoRate = sourceFrameRate(*impl_->videoStream);
    decoder::DecoderConfig videoConfig = impl_->decoderConfig;
    if (videoConfig.preferred == decoder::DecoderBackend::D3D11) {
        renderer::D3D11Context::instance().createDevice();
        videoConfig.allowFallback = true;
    }
    impl_->videoDecoder = std::make_unique<pipeline::DecoderNode>();
    if (impl_->videoDecoder->open(*impl_->videoStream, videoConfig) < 0) {
        setError(error, "Failed to create video decoder");
        impl_->resetPipeline();
        return false;
    }

    if (impl_->audioStream) {
        impl_->audioDecoder = std::make_unique<pipeline::DecoderNode>();
        if (impl_->audioDecoder->open(*impl_->audioStream, impl_->decoderConfig) < 0) {
            LOG_WARN("Producer: failed to create audio decoder");
            impl_->audioDecoder.reset();
            impl_->audioStream = nullptr;
        }
    }

    impl_->length = impl_->profile.frameFromSeconds(impl_->durationSeconds);
    if (impl_->length <= 0 && impl_->durationSeconds > 0.0) impl_->length = 1;
    if (impl_->length <= 0) {
        setError(error, "Media duration is empty");
        impl_->resetPipeline();
        return false;
    }

    std::string normalizeError;
    impl_->normalizer = acquireNormalizer(impl_->profile, &normalizeError);
    if (!impl_->normalizer) {
        setError(error, "Failed to initialize Profile canvas: " + normalizeError);
        impl_->resetPipeline();
        return false;
    }

    if (!impl_->jumpTo(0)) {
        setError(error, "Failed to decode the first frame");
        impl_->resetPipeline();
        return false;
    }

    impl_->position = 0;
    LOG_INFO("Producer: opened {} — {} frames, {:.3f}s",
             path, impl_->length, impl_->durationSeconds);
    return true;
}

void Producer::close() {
    impl_->resource.clear();
    impl_->resetPipeline();
}

bool Producer::isOpen() const {
    return impl_->demux && impl_->demux->isOpen();
}

const Profile& Producer::profile() const {
    return impl_->profile;
}

const std::string& Producer::resource() const {
    return impl_->resource;
}

int64_t Producer::in() const {
    return 0;
}

int64_t Producer::out() const {
    return impl_->length > 0 ? impl_->length - 1 : 0;
}

int64_t Producer::length() const {
    return impl_->length;
}

int64_t Producer::position() const {
    return impl_->position;
}

bool Producer::seekable() const {
    return impl_->seekableMedia;
}

bool Producer::seek(int64_t position) {
    if (!isOpen() || impl_->length <= 0) return false;
    impl_->position = impl_->clampPosition(position);
    return true;
}

ProducerFrame Producer::getFrame(int64_t position) {
    ProducerFrame frame;
    if (!isOpen() || impl_->length <= 0) {
        frame.eof = true;
        return frame;
    }

    const int64_t clamped = impl_->clampPosition(position);
    frame.position = clamped;
    if (position < 0 || position >= impl_->length) {
        frame.eof = true;
        impl_->position = clamped;
        return frame;
    }

    if (!impl_->jumpTo(clamped)) {
        frame.eof = true;
        impl_->position = clamped;
        return frame;
    }

    std::string normalizeError;
    if (!impl_->lastVideo) {
        frame.eof = true;
        impl_->position = clamped;
        return frame;
    }
    impl_->lastNormalized = impl_->normalizer->normalize(
        impl_->lastVideo.get(), &normalizeError);
    if (!impl_->lastNormalized) {
        LOG_ERROR("Producer: normalize failed: {}", normalizeError);
        frame.eof = true;
        impl_->position = clamped;
        return frame;
    }
    impl_->lastNormalized->pts = clamped;
    impl_->lastNormalized->time_base = {
        impl_->profile.timeBase().num,
        impl_->profile.timeBase().den
    };
    impl_->lastNormalized->duration = 1;
    frame.video = impl_->lastNormalized;
    if (impl_->audioDecoder) frame.audio = impl_->takeAudio(clamped);
    impl_->position = clamped;
    return frame;
}

} // namespace heisenberg
