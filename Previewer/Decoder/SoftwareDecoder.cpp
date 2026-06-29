//
// Created by NiceFold on 2026/6/30.
//

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
}

#include <Decoder/SoftwareDecoder.hpp>
#include <Common/Codec.hpp>
#include <Common/Packet.hpp>
#include <Common/Stream.hpp>

#include <memory>

namespace heisenberg {
namespace decoder {

// ============================================================
// AVFrame 自定义删除器，供 shared_ptr 使用
// ============================================================
namespace {
void avframeDeleter(AVFrame* frame) {
    av_frame_free(&frame);
}

/// 将项目内部的 CodecParams::ID 映射回 FFmpeg AVCodecID。
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

// ============================================================
// Impl
// ============================================================
struct SoftwareDecoder::Impl {
    const AVCodec* codec = nullptr;
    AVCodecContext* ctx = nullptr;
    bool isOpen_ = false;
    int pixelFormat_ = AV_PIX_FMT_NONE;

    void close() {
        if (ctx) {
            avcodec_free_context(&ctx);
            ctx = nullptr;
        }
        codec = nullptr;
        isOpen_ = false;
        pixelFormat_ = AV_PIX_FMT_NONE;
    }
};

// ============================================================
// 生命周期
// ============================================================
SoftwareDecoder::SoftwareDecoder()
    : impl_(std::make_unique<Impl>()) {}

SoftwareDecoder::~SoftwareDecoder() {
    impl_->close();
}

int SoftwareDecoder::open(const Stream& stream) {
    impl_->close();

    // 1. 找到对应的 FFmpeg 软件解码器
    AVCodecID avCodecId = toAVCodecID(stream.codec.codecId);
    if (avCodecId == AV_CODEC_ID_NONE) {
        return -1;
    }

    impl_->codec = avcodec_find_decoder(avCodecId);
    if (!impl_->codec) {
        return -2;
    }

    // 2. 分配解码上下文
    impl_->ctx = avcodec_alloc_context3(impl_->codec);
    if (!impl_->ctx) {
        impl_->close();
        return -3;
    }

    // 3. 从 Stream 填充解码参数
    impl_->ctx->width = stream.codec.width;
    impl_->ctx->height = stream.codec.height;
    impl_->ctx->pix_fmt = AV_PIX_FMT_NONE; // 让 FFmpeg 自动选择

    // 时间基
    impl_->ctx->time_base = {stream.codec.tbNum, stream.codec.tbDen};
    impl_->ctx->framerate = {stream.codec.fpsNum, stream.codec.fpsDen};

    // extradata（H.264 的 SPS/PPS 等）
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

    // 4. 打开解码器
    int ret = avcodec_open2(impl_->ctx, impl_->codec, nullptr);
    if (ret < 0) {
        impl_->close();
        return ret;
    }

    // 注意：pixelFormat_ 此时可能为 AV_PIX_FMT_NONE（H.264 等需首帧解码后才确定）。
    // receiveFrame 会在首帧时自动更新。
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

// ============================================================
// 解码
// ============================================================
int SoftwareDecoder::sendPacket(std::shared_ptr<const Packet> packet) {
    if (!impl_->isOpen_) {
        return -1;
    }

    if (!packet || packet->empty()) {
        // 空包表示 flush：通知解码器不再有输入数据，取完缓冲帧
        return avcodec_send_packet(impl_->ctx, nullptr);
    }

    AVPacket avpkt = {};
    avpkt.data = const_cast<uint8_t*>(packet->data());
    avpkt.size = packet->size();

    if (packet->pts >= 0.0) {
        avpkt.pts = static_cast<int64_t>(packet->pts / av_q2d(impl_->ctx->time_base));
    } else {
        avpkt.pts = AV_NOPTS_VALUE;
    }

    if (packet->dts >= 0.0) {
        avpkt.dts = static_cast<int64_t>(packet->dts / av_q2d(impl_->ctx->time_base));
    } else {
        avpkt.dts = AV_NOPTS_VALUE;
    }

    if (packet->keyframe) {
        avpkt.flags |= AV_PKT_FLAG_KEY;
    }

    return avcodec_send_packet(impl_->ctx, &avpkt);
}

std::shared_ptr<AVFrame> SoftwareDecoder::receiveFrame() {
    if (!impl_->isOpen_) {
        return nullptr;
    }

    AVFrame* raw = av_frame_alloc();
    if (!raw) {
        return nullptr;
    }

    int ret = avcodec_receive_frame(impl_->ctx, raw);
    if (ret < 0) {
        av_frame_free(&raw);
        return nullptr; // EAGAIN（需要更多包）或 EOF 或其他错误
    }

    // 首帧：更新运行时确定的 pixelFormat
    if (impl_->pixelFormat_ == AV_PIX_FMT_NONE && raw->format != AV_PIX_FMT_NONE) {
        impl_->pixelFormat_ = raw->format;
    }

    // PTS 从 time_base 换算为毫秒（保持整数精度）
    if (raw->pts != AV_NOPTS_VALUE) {
        raw->pts = av_rescale_q(raw->pts, impl_->ctx->time_base, {1, 1000});
    }

    return std::shared_ptr<AVFrame>(raw, avframeDeleter);
}

// ============================================================
// Seek 支持
// ============================================================
void SoftwareDecoder::flush() {
    if (impl_->ctx) {
        avcodec_flush_buffers(impl_->ctx);
    }
}

// ============================================================
// 能力查询
// ============================================================
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
