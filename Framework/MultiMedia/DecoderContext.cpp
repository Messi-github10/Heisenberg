#include <MultiMedia/DecoderContext.hpp>

#include <Common/AudioFrame.hpp>
#include <Common/AudioSpec.hpp>
#include <Common/Packet.hpp>
#include <Common/Stream.hpp>
#include <Common/VideoFrame.hpp>
#include <MultiMedia/Video/Decoder/IDecoder.hpp>
#include <Utiles/Logger.hpp>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
#include <libswresample/swresample.h>
}

#include <cstring>
#include <vector>

namespace heisenberg::decoder {

struct DecoderContext::AudioConverter {
    SwrContext* context = nullptr;
    AVSampleFormat inputFormat = AV_SAMPLE_FMT_NONE;
    int sampleRate = 0;
    int channels = 0;

    ~AudioConverter() {
        swr_free(&context);
    }

    bool configure(const AVFrame& frame) {
        const int frameChannels = frame.ch_layout.nb_channels;
        if (context && inputFormat == frame.format &&
            sampleRate == frame.sample_rate && channels == frameChannels) {
            return true;
        }

        swr_free(&context);
        inputFormat = static_cast<AVSampleFormat>(frame.format);
        sampleRate = frame.sample_rate;
        channels = frameChannels;
        if (sampleRate <= 0 || channels <= 0) return false;

        AVChannelLayout inputLayout = frame.ch_layout;
        AVChannelLayout defaultLayout{};
        if (inputLayout.nb_channels <= 0) {
            av_channel_layout_default(&defaultLayout, channels);
            inputLayout = defaultLayout;
        }

        AVChannelLayout outputLayout{};
        av_channel_layout_default(&outputLayout, channels);
        const int result = swr_alloc_set_opts2(
            &context,
            &outputLayout, AV_SAMPLE_FMT_FLTP, sampleRate,
            &inputLayout, inputFormat, sampleRate,
            0, nullptr);
        av_channel_layout_uninit(&outputLayout);
        av_channel_layout_uninit(&defaultLayout);
        if (result < 0 || !context || swr_init(context) < 0) {
            swr_free(&context);
            return false;
        }
        return true;
    }

    AudioFramePtr convert(const AVFrame& frame) {
        if (!configure(frame)) return {};

        const int maxSamples = static_cast<int>(av_rescale_rnd(
            swr_get_delay(context, sampleRate) + frame.nb_samples,
            sampleRate, sampleRate, AV_ROUND_UP));
        if (maxSamples <= 0) return {};

        std::vector<float> converted(
            static_cast<size_t>(maxSamples) * channels);
        std::vector<uint8_t*> planes(static_cast<size_t>(channels));
        for (int channel = 0; channel < channels; ++channel) {
            planes[channel] = reinterpret_cast<uint8_t*>(
                converted.data() + static_cast<size_t>(channel) * maxSamples);
        }

        const int samples = swr_convert(
            context, planes.data(), maxSamples,
            const_cast<const uint8_t**>(frame.extended_data),
            frame.nb_samples);
        if (samples <= 0) return {};

        auto output = std::make_shared<AudioFrame>();
        AudioSpec spec;
        spec.sampleRate = sampleRate;
        spec.channels = channels;
        spec.layout = defaultLayout(channels);
        output->setSpec(spec);
        output->setSamples(samples, true);
        for (int channel = 0; channel < channels; ++channel) {
            std::memcpy(output->channelRw(channel),
                        converted.data() +
                            static_cast<size_t>(channel) * maxSamples,
                        static_cast<size_t>(samples) * sizeof(float));
        }

        if (frame.pts != AV_NOPTS_VALUE && frame.time_base.num > 0 &&
            frame.time_base.den > 0) {
            output->setPosition(av_rescale_q(
                frame.pts, frame.time_base, AVRational{1, sampleRate}));
        }
        return output;
    }
};

DecoderContext::DecoderContext() = default;
DecoderContext::~DecoderContext() = default;

int DecoderContext::open(const Stream& stream,
                      const decoder::DecoderConfig& config) {
    close();
    // Hardware video backends are not audio decoders. Keep audio on the
    // software path even when the user enables D3D11 video decoding.
    decoder::DecoderConfig effectiveConfig = config;
    if (stream.isAudio()) {
        effectiveConfig.preferred = decoder::DecoderBackend::Software;
        effectiveConfig.allowFallback = false;
    }
    decoder_ = decoder::createDecoder(effectiveConfig);
    if (!decoder_) return -1;

    int result = decoder_->open(stream);
    if (result < 0 && effectiveConfig.allowFallback
        && effectiveConfig.preferred != decoder::DecoderBackend::Software) {
        LOG_WARN("DecoderContext: preferred decoder failed for stream {}, "
                 "falling back to software", stream.index);
        decoder_.reset();
        decoder::DecoderConfig softwareConfig;
        softwareConfig.preferred = decoder::DecoderBackend::Software;
        softwareConfig.allowFallback = false;
        decoder_ = decoder::createDecoder(softwareConfig);
        if (decoder_) result = decoder_->open(stream);
    }
    if (result < 0) {
        decoder_.reset();
        return result;
    }

    streamIndex_ = stream.index;
    audio_ = stream.isAudio();
    if (audio_) audioConverter_ = std::make_unique<AudioConverter>();
    return 0;
}

void DecoderContext::close() {
    if (decoder_) decoder_->close();
    decoder_.reset();
    audioConverter_.reset();
    streamIndex_ = -1;
    audio_ = false;
    draining_ = false;
    eofReturned_ = false;
    pendingFrames_.clear();
}

void DecoderContext::flush() {
    if (decoder_) decoder_->flush();
    if (audio_) audioConverter_ = std::make_unique<AudioConverter>();
    draining_ = false;
    eofReturned_ = false;
    pendingFrames_.clear();
}

bool DecoderContext::push(const MediaFrame& input) {
    if (!decoder_) return false;

    PacketPtr packet;
    const bool drain = input.type == MediaFrameType::Eof;
    if (input.type == MediaFrameType::Packet) {
        packet = std::get<PacketPtr>(input.payload);
        if (!packet || packet->streamIndex != streamIndex_) return false;
    } else if (!drain) return false;

    int result;
    while ((result = decoder_->sendPacket(packet)) == AVERROR(EAGAIN)) {
        auto frame = decoder_->receiveFrame();
        if (!frame) return false; // The FFmpeg send/receive contract forbids this.
        pendingFrames_.push_back(std::move(frame));
    }
    draining_ = drain && (result >= 0 || result == AVERROR_EOF);
    eofReturned_ = false;
    return result >= 0 || (drain && result == AVERROR_EOF);
}

MediaFrame DecoderContext::pull(uint64_t generation) {
    if (!decoder_) return {};

    auto frame = pendingFrames_.empty() ? decoder_->receiveFrame() : std::move(pendingFrames_.front());
    if (!pendingFrames_.empty()) pendingFrames_.pop_front();
    if (frame) {
        MediaFrame output;
        if (audio_) {
            auto audioFrame = audioConverter_->convert(*frame);
            if (!audioFrame) return {};
            output = MediaFrame::audio(std::move(audioFrame), generation);
        } else {
            output = MediaFrame::video(VideoFrame::wrap(frame), generation);
        }
        output.metadata.pts = frame->pts;
        output.metadata.duration = frame->duration;
        output.metadata.timeBaseNum = frame->time_base.num;
        output.metadata.timeBaseDen = frame->time_base.den;
        output.metadata.streamIndex = streamIndex_;
        return output;
    }

    if (draining_ && !eofReturned_) {
        eofReturned_ = true;
        draining_ = false;
        return MediaFrame::eof(generation);
    }

    return {};
}

bool DecoderContext::isOpen() const {
    return decoder_ && decoder_->isOpen();
}

} // namespace heisenberg::decoder
