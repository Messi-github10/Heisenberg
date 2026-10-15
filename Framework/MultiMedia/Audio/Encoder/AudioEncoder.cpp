#include "AudioEncoder.hpp"
#include <Common/AudioFrame.hpp>
#include <MultiMedia/Video/Encoder/EncoderInternal.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

extern "C" {
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/samplefmt.h>
}

namespace heisenberg {
using namespace encoding_detail;
namespace {
struct FifoDeleter { void operator()(AVAudioFifo* p) const { av_audio_fifo_free(p); } };
void skipSamples(AVPacket& packet, uint32_t start, uint32_t end) {
    size_t size = 0;
    uint8_t* data = av_packet_get_side_data(&packet, AV_PKT_DATA_SKIP_SAMPLES, &size);
    if (data && size < 10) throw EncodingError("invalid encoder skip-samples side data");
    if (!data) {
        data = av_packet_new_side_data(&packet, AV_PKT_DATA_SKIP_SAMPLES, 10);
        if (!data) throw std::bad_alloc();
        std::memset(data, 0, 10);
    }
    AV_WL32(data, std::max<uint32_t>(static_cast<uint32_t>(AV_RL32(data)), start));
    AV_WL32(data + 4, std::max<uint32_t>(static_cast<uint32_t>(AV_RL32(data + 4)), end));
}
}
struct AudioEncoder::Impl {
    CodecPtr context;
    std::unique_ptr<AVAudioFifo, FifoDeleter> fifo;
    PacketPtr pending;
    int64_t total = 0, nextPts = 0;
    bool firstPacket = true, drained = false, failed = false;
    Impl() {
        const AVCodec* codec = avcodec_find_encoder_by_name("aac");
        if (!codec) throw EncodingError("AAC encoder unavailable");
        context.reset(avcodec_alloc_context3(codec));
        if (!context) throw std::bad_alloc();
        context->sample_fmt = AV_SAMPLE_FMT_FLTP;
        context->sample_rate = 48000;
        av_channel_layout_default(&context->ch_layout, 2);
        context->time_base = {1, 48000};
        context->bit_rate = 192000;
        context->profile = AV_PROFILE_AAC_LOW;
        context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        check(avcodec_open2(context.get(), codec, nullptr), "avcodec_open2(audio)");
        if (context->frame_size <= 0) throw EncodingError("AAC encoder has invalid frame size");
        if (context->sample_fmt != AV_SAMPLE_FMT_FLTP || context->sample_rate != 48000 || context->ch_layout.nb_channels != 2)
            throw EncodingError("AAC encoder changed the required PCM format");
        fifo.reset(av_audio_fifo_alloc(context->sample_fmt, 2, context->frame_size));
        if (!fifo) throw std::bad_alloc();
    }
    void state() const {
        if (failed) throw EncodingError("audio encoder failed; discard this instance");
        if (drained) throw EncodingError("audio encoder already drained");
    }
    void receive(AVPacket& packet, EncodedPackets& output) {
        if (packet.pts == AV_NOPTS_VALUE) throw EncodingError("AAC encoder returned unknown PTS");
        if (firstPacket) {
            if (context->initial_padding > 0) skipSamples(packet, static_cast<uint32_t>(context->initial_padding), 0);
            firstPacket = false;
        }
        if (pending) output.emplace_back(*pending, context->time_base);
        pending = encoding_detail::packet();
        check(av_packet_ref(pending.get(), &packet), "av_packet_ref(audio pending)");
    }
    void sendPcm(int realSamples, EncodedPackets& output) {
        const bool smallLast = context->codec->capabilities & AV_CODEC_CAP_SMALL_LAST_FRAME;
        const int encodedSamples = smallLast ? realSamples : context->frame_size;
        auto input = frame();
        input->format = context->sample_fmt;
        input->sample_rate = context->sample_rate;
        check(av_channel_layout_copy(&input->ch_layout, &context->ch_layout), "av_channel_layout_copy(audio)");
        input->nb_samples = encodedSamples;
        input->pts = nextPts;
        input->duration = realSamples;
        check(av_frame_get_buffer(input.get(), 0), "av_frame_get_buffer(audio)");
        if (encodedSamples > realSamples)
            check(av_samples_set_silence(input->extended_data, 0, encodedSamples, 2, AV_SAMPLE_FMT_FLTP), "pad audio frame");
        const int count = av_audio_fifo_read(fifo.get(), reinterpret_cast<void**>(input->extended_data), realSamples);
        if (count != realSamples) throw EncodingError("audio FIFO short read");
        send(context.get(), input.get(), [&](AVPacket& packet) { receive(packet, output); });
        nextPts += realSamples;
    }
};

AudioEncoder::AudioEncoder() : impl_(std::make_unique<Impl>()) {}
AudioEncoder::~AudioEncoder() = default;
CodecParameters AudioEncoder::codecParameters() const { return CodecParameters(*impl_->context); }
AVRational AudioEncoder::timeBase() const noexcept { return impl_->context->time_base; }
int AudioEncoder::frameSize() const noexcept { return impl_->context->frame_size; }
int AudioEncoder::initialPadding() const noexcept { return impl_->context->initial_padding; }
int64_t AudioEncoder::samplesSubmitted() const noexcept { return impl_->total; }
EncodedPackets AudioEncoder::encode(const float* left, const float* right, int samples) {
    impl_->state();
    require(samples >= 0, "negative PCM sample count");
    if (samples == 0) return {};
    require(left && right, "PCM channel data is null");
    require(impl_->total <= std::numeric_limits<int64_t>::max() - samples - impl_->context->frame_size,
            "PCM sample count overflows timestamps");
    require(samples <= std::numeric_limits<int>::max() - av_audio_fifo_size(impl_->fifo.get()), "PCM FIFO capacity overflow");
    for (int s = 0; s < samples; ++s)
        require(std::isfinite(left[s]) && std::isfinite(right[s]), "PCM contains NaN/Inf");
    EncodedPackets packets;
    try {
        check(av_audio_fifo_realloc(impl_->fifo.get(), av_audio_fifo_size(impl_->fifo.get()) + samples), "av_audio_fifo_realloc");
        void* channels[] = {const_cast<float*>(left), const_cast<float*>(right)};
        const int count = av_audio_fifo_write(impl_->fifo.get(), channels, samples);
        if (count != samples) throw EncodingError("audio FIFO short write");
        impl_->total += samples;
        while (av_audio_fifo_size(impl_->fifo.get()) >= frameSize()) impl_->sendPcm(frameSize(), packets);
    } catch (...) { impl_->failed = true; throw; }
    return packets;
}
EncodedPackets AudioEncoder::encode(const AudioFrame& pcm) {
    require(pcm.spec().sampleRate == 48000 && pcm.spec().channels == 2, "PCM must be 48 kHz stereo");
    require(pcm.spec().layout == ChannelLayout::Auto || pcm.spec().layout == ChannelLayout::Stereo,
            "PCM must have stereo channel layout");
    require(!pcm.hasPosition() || pcm.position() == impl_->total, "PCM position is not contiguous from zero");
    require(pcm.samples() >= 0 && (pcm.samples() == 0 || !pcm.empty()), "invalid PCM AudioFrame buffer");
    if (pcm.samples() == 0) return encode(nullptr, nullptr, 0);
    return encode(pcm.channelRo(0), pcm.channelRo(1), pcm.samples());
}
EncodedPackets AudioEncoder::encode(const AVFrame& pcm) {
    require(pcm.format == AV_SAMPLE_FMT_FLTP && pcm.sample_rate == 48000, "AVFrame PCM must be 48 kHz FLTP");
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    require(av_channel_layout_compare(&pcm.ch_layout, &stereo) == 0, "AVFrame PCM must have stereo channel layout");
    require(pcm.pts == AV_NOPTS_VALUE || pcm.pts == impl_->total, "PCM PTS must be contiguous in 1/48000");
    require(pcm.extended_data != nullptr && pcm.nb_samples >= 0, "invalid PCM AVFrame");
    if (pcm.nb_samples == 0) return encode(nullptr, nullptr, 0);
    require(static_cast<int64_t>(pcm.linesize[0]) >= static_cast<int64_t>(pcm.nb_samples) * static_cast<int64_t>(sizeof(float)),
            "PCM AVFrame stride is too small");
    return encode(reinterpret_cast<const float*>(pcm.extended_data[0]),
                  reinterpret_cast<const float*>(pcm.extended_data[1]), pcm.nb_samples);
}
EncodedPackets AudioEncoder::drain() {
    if (impl_->failed) throw EncodingError("audio encoder failed; discard this instance");
    if (impl_->drained) return {};
    EncodedPackets packets;
    try {
        const int remaining = av_audio_fifo_size(impl_->fifo.get());
        if (remaining) impl_->sendPcm(remaining, packets);
        send(impl_->context.get(), nullptr, [&](AVPacket& packet) { impl_->receive(packet, packets); });
        if (impl_->pending) {
            auto& last = *impl_->pending;
            // AAC decodes frame_size samples even with a partial last input.
            // Duration is real media; skips describe coded padding independently.
            const int64_t realEnd = impl_->total - last.pts;
            require(realEnd >= 0 && realEnd <= frameSize(), "unexpected AAC last packet timeline");
            const auto padding = static_cast<uint32_t>(frameSize() - realEnd);
            skipSamples(last, 0, padding);
            last.duration = realEnd;
            impl_->context->trailing_padding = static_cast<int>(padding);
            packets.emplace_back(last, timeBase());
            impl_->pending.reset();
        } else if (impl_->total != 0) throw EncodingError("AAC encoder emitted no packets");
        impl_->drained = true;
    } catch (...) { impl_->failed = true; throw; }
    return packets;
}
} // namespace heisenberg
