//
// Created by NiceFold on 2026/6/30.
//

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
}

#include <MultiMedia/Video/Decoder/SoftwareDecoder.hpp>
#include <Common/Codec.hpp>
#include <Common/Packet.hpp>
#include <Common/Stream.hpp>

#include <memory>

namespace heisenberg {
namespace decoder {

namespace {
void avframeDeleter(AVFrame* frame) {
    av_frame_free(&frame);
}

AVCodecID toAVCodecID(CodecParams::ID id) {
    switch (id) {
    case CodecParams::H264:   return AV_CODEC_ID_H264;
    case CodecParams::HEVC:   return AV_CODEC_ID_HEVC;
    case CodecParams::VP9:    return AV_CODEC_ID_VP9;
    case CodecParams::AV1:    return AV_CODEC_ID_AV1;
    case CodecParams::AAC:    return AV_CODEC_ID_AAC;
    case CodecParams::MP3:    return AV_CODEC_ID_MP3;
    case CodecParams::OPUS:   return AV_CODEC_ID_OPUS;
    case CodecParams::FLAC:   return AV_CODEC_ID_FLAC;
    case CodecParams::VORBIS: return AV_CODEC_ID_VORBIS;
    default:                  return AV_CODEC_ID_NONE;
    }
}
} // namespace

struct SoftwareDecoder::Impl {
    const AVCodec* codec = nullptr;
    AVCodecContext* ctx = nullptr;
    bool isOpen_ = false;
    int pixelFormat_ = AV_PIX_FMT_NONE;
    int64_t startTime_ = 0;

    int lastError = 0;
    int record(int result) {
        if (result < 0 && result != AVERROR(EAGAIN) && result != AVERROR_EOF) lastError = result;
        return result;
    }

    void close() {
        lastError = 0;
        if (ctx) {
            avcodec_free_context(&ctx);
            ctx = nullptr;
        }
        codec = nullptr;
        isOpen_ = false;
        pixelFormat_ = AV_PIX_FMT_NONE;
        startTime_ = 0;
    }
};

SoftwareDecoder::SoftwareDecoder()
    : impl_(std::make_unique<Impl>()) {}

SoftwareDecoder::~SoftwareDecoder() {
    impl_->close();
}

int SoftwareDecoder::open(const Stream& stream) {
    impl_->close();

    AVCodecID avCodecId = stream.codec.nativeCodecId != 0
        ? static_cast<AVCodecID>(stream.codec.nativeCodecId) : toAVCodecID(stream.codec.codecId);
    if (avCodecId == AV_CODEC_ID_NONE) {
        return -1;
    }

    impl_->codec = avcodec_find_decoder(avCodecId);
    if (!impl_->codec) {
        return -2;
    }

    impl_->ctx = avcodec_alloc_context3(impl_->codec);
    if (!impl_->ctx) {
        impl_->close();
        return -3;
    }

    impl_->ctx->width = stream.codec.width();
    impl_->ctx->height = stream.codec.height();
    impl_->ctx->pix_fmt = AV_PIX_FMT_NONE;
    if (stream.isAudio()) {
        impl_->ctx->sample_rate = stream.codec.sampleRate();
        if (stream.codec.audio.channelMask)
            av_channel_layout_from_mask(&impl_->ctx->ch_layout, stream.codec.audio.channelMask);
        else if (stream.codec.channels() > 0)
            av_channel_layout_default(&impl_->ctx->ch_layout, stream.codec.channels());
        impl_->ctx->sample_fmt = static_cast<AVSampleFormat>(stream.codec.audio.sampleFormat);
        impl_->ctx->block_align = stream.codec.audio.blockAlign;
        impl_->ctx->bits_per_coded_sample = stream.codec.audio.codedBitDepth;
        impl_->ctx->bits_per_raw_sample = stream.codec.audio.bitDepth;
    }
    impl_->ctx->codec_tag = stream.codec.codecTag;
    impl_->ctx->bit_rate = stream.codec.bitRate;

    impl_->ctx->time_base = {stream.codec.tbNum(), stream.codec.tbDen()};
    impl_->ctx->pkt_timebase = impl_->ctx->time_base;
    impl_->ctx->framerate = {stream.codec.fpsNum(), stream.codec.fpsDen()};
    impl_->startTime_ = stream.originKnown ? stream.originTime : stream.startTime;

    if (stream.codec.extradata() && stream.codec.extradataSize() > 0) {
        impl_->ctx->extradata = static_cast<uint8_t*>(
            av_mallocz(stream.codec.extradataSize() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!impl_->ctx->extradata) {
            impl_->close();
            return -4;
        }
        memcpy(impl_->ctx->extradata, stream.codec.extradata(),
               stream.codec.extradataSize());
        impl_->ctx->extradata_size = stream.codec.extradataSize();
    }

    int ret = avcodec_open2(impl_->ctx, impl_->codec, nullptr);
    if (ret < 0) {
        impl_->close();
        return ret;
    }

    impl_->pixelFormat_ = impl_->ctx->pix_fmt;
    impl_->isOpen_ = true;
    return 0;
}

void SoftwareDecoder::close() {
    impl_->close();
}

bool SoftwareDecoder::isOpen() const {
    return impl_->isOpen_;
}

int SoftwareDecoder::sendPacket(std::shared_ptr<const Packet> packet) {
    if (!impl_->isOpen_) {
        return impl_->record(AVERROR(EINVAL));
    }

    if (!packet || packet->empty()) {
        return impl_->record(avcodec_send_packet(impl_->ctx, nullptr));
    }

    AVPacket avpkt = {};
    avpkt.data = const_cast<uint8_t*>(packet->data());
    avpkt.size = packet->size();

    AVRational packetTimeBase = {
        packet->timeBaseNum,
        packet->timeBaseDen
    };
    if (packetTimeBase.num <= 0 || packetTimeBase.den <= 0) {
        packetTimeBase = impl_->ctx->pkt_timebase;
    }

    if (packet->hasPts) {
        avpkt.pts = av_rescale_q(packet->pts,
                                packetTimeBase,
                                impl_->ctx->pkt_timebase);
    } else {
        avpkt.pts = AV_NOPTS_VALUE;
    }

    if (packet->hasDts) {
        avpkt.dts = av_rescale_q(packet->dts,
                                packetTimeBase,
                                impl_->ctx->pkt_timebase);
    } else {
        avpkt.dts = AV_NOPTS_VALUE;
    }
    avpkt.duration = av_rescale_q(packet->duration,
                                  packetTimeBase,
                                  impl_->ctx->pkt_timebase);
    avpkt.pos = packet->filePos;

    if (packet->keyframe) {
        avpkt.flags |= AV_PKT_FLAG_KEY;
    }

    // FFmpeg consumes skip/discard sample side data itself (including MP3/AAC
    // encoder delay). Keep it attached on EAGAIN retries as well.
    for (const auto& data : packet->sideData) {
        auto* copy = av_packet_new_side_data(&avpkt,
            static_cast<AVPacketSideDataType>(data.type), data.data.size());
        if (!copy) { av_packet_free_side_data(&avpkt); return impl_->record(AVERROR(ENOMEM)); }
        std::memcpy(copy, data.data.data(), data.data.size());
    }
    const int result = avcodec_send_packet(impl_->ctx, &avpkt);
    av_packet_free_side_data(&avpkt);
    return impl_->record(result);
}

std::shared_ptr<AVFrame> SoftwareDecoder::receiveFrame() {
    if (!impl_->isOpen_) {
        impl_->record(AVERROR(EINVAL));
        return nullptr;
    }

    AVFrame* raw = av_frame_alloc();
    if (!raw) {
        impl_->record(AVERROR(ENOMEM));
        return nullptr;
    }

    int ret = avcodec_receive_frame(impl_->ctx, raw);
    impl_->record(ret);
    if (ret < 0) {
        av_frame_free(&raw);
        return nullptr;
    }

    if (impl_->pixelFormat_ == AV_PIX_FMT_NONE && raw->format != AV_PIX_FMT_NONE) {
        impl_->pixelFormat_ = raw->format;
    }

    int64_t displayPts = raw->best_effort_timestamp;
    if (displayPts == AV_NOPTS_VALUE) {
        displayPts = raw->pts;
    }
    raw->time_base = impl_->ctx->pkt_timebase;
    if (displayPts != AV_NOPTS_VALUE) {
        // Keep the zero-based display timestamp in the stream's native units.
        // best_effort_timestamp retains FFmpeg's original absolute timestamp.
        raw->pts = displayPts - impl_->startTime_;
    }

    return std::shared_ptr<AVFrame>(raw, avframeDeleter);
}

int SoftwareDecoder::lastError() const { return impl_->lastError; }

void SoftwareDecoder::flush() {
    impl_->lastError = 0;
    if (impl_->ctx) {
        avcodec_flush_buffers(impl_->ctx);
    }
}

DecoderBackend SoftwareDecoder::backend() const {
    return DecoderBackend::Software;
}

bool SoftwareDecoder::isHardware() const {
    return false;
}

int SoftwareDecoder::outputPixelFormat() const {
    return impl_->pixelFormat_;
}

} // namespace decoder
} // namespace heisenberg
