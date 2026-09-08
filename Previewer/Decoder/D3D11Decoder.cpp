#include <Decoder/D3D11Decoder.hpp>

#include <Common/Codec.hpp>
#include <Common/Packet.hpp>
#include <Common/Stream.hpp>
#include <Renderer/D3D11Context.hpp>
#include <Utiles/Logger.hpp>

#include <windows.h>
#include <d3d11.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
}

#include <cstring>

namespace heisenberg::decoder {
namespace {

AVCodecID toAVCodecID(CodecParams::ID id) {
    switch (id) {
    case CodecParams::H264: return AV_CODEC_ID_H264;
    case CodecParams::HEVC: return AV_CODEC_ID_HEVC;
    case CodecParams::VP9: return AV_CODEC_ID_VP9;
    case CodecParams::AV1: return AV_CODEC_ID_AV1;
    default: return AV_CODEC_ID_NONE;
    }
}

void avframeDeleter(AVFrame* frame) { av_frame_free(&frame); }

AVPixelFormat selectD3D11Format(AVCodecContext*, const AVPixelFormat* formats) {
    for (const AVPixelFormat* format = formats;
         format && *format != AV_PIX_FMT_NONE; ++format) {
        if (*format == AV_PIX_FMT_D3D11) return *format;
    }
    return AV_PIX_FMT_NONE;
}

} // namespace

struct D3D11Decoder::Impl {
    const AVCodec* codec = nullptr;
    AVCodecContext* ctx = nullptr;
    AVBufferRef* hwDeviceCtx = nullptr;
    AVBufferRef* hwFramesCtx = nullptr;
    bool open = false;
    int pixelFormat = AV_PIX_FMT_NONE;
    int64_t startTime = 0;

    void close() {
        if (ctx) avcodec_free_context(&ctx);
        if (hwFramesCtx) av_buffer_unref(&hwFramesCtx);
        if (hwDeviceCtx) av_buffer_unref(&hwDeviceCtx);
        codec = nullptr;
        open = false;
        pixelFormat = AV_PIX_FMT_NONE;
        startTime = 0;
    }
};

D3D11Decoder::D3D11Decoder() : impl_(std::make_unique<Impl>()) {}
D3D11Decoder::~D3D11Decoder() { impl_->close(); }

int D3D11Decoder::open(const Stream& stream) {
    impl_->close();
    const AVCodecID codecId = toAVCodecID(stream.codec.codecId);
    if (codecId == AV_CODEC_ID_NONE) return -1;
    ID3D11Device* device = renderer::D3D11Context::instance().device();
    if (!device) return -2;

    impl_->codec = avcodec_find_decoder(codecId);
    if (!impl_->codec) return -3;
    bool supportsD3D11 = false;
    for (int index = 0;; ++index) {
        const AVCodecHWConfig* config = avcodec_get_hw_config(impl_->codec, index);
        if (!config) break;
        if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
            config->device_type == AV_HWDEVICE_TYPE_D3D11VA) {
            supportsD3D11 = true;
            break;
        }
    }
    if (!supportsD3D11) { impl_->close(); return -4; }

    AVBufferRef* deviceRef = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!deviceRef) return -5;
    auto* deviceContext = reinterpret_cast<AVD3D11VADeviceContext*>(
        reinterpret_cast<AVHWDeviceContext*>(deviceRef->data)->hwctx);
    if (!deviceContext) {
        av_buffer_unref(&deviceRef);
        return -5;
    }
    // Supplying the device is sufficient.  FFmpeg obtains and owns the
    // immediate/video contexts during av_hwdevice_ctx_init().  Passing our
    // immediate context here is unnecessary and can expose a stale context
    // across decoder-thread initialization.
    // FFmpeg releases AVD3D11VADeviceContext::device when the AVHW device
    // context is destroyed. Retain a reference for that ownership.
    device->AddRef();
    deviceContext->device = device;
    // The decoded NV12/P010 surfaces are sampled by libplacebo for the
    // D3D11-side color conversion, so they must expose both decoder and
    // shader-resource bindings.
    deviceContext->BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;
    deviceContext->MiscFlags = 0;
    if (av_hwdevice_ctx_init(deviceRef) < 0) {
        av_buffer_unref(&deviceRef);
        return -6;
    }
    impl_->hwDeviceCtx = deviceRef;

    impl_->ctx = avcodec_alloc_context3(impl_->codec);
    if (!impl_->ctx) { impl_->close(); return -7; }
    impl_->ctx->width = stream.codec.width;
    impl_->ctx->height = stream.codec.height;
    impl_->ctx->pix_fmt = AV_PIX_FMT_NONE;
    // Hardware frames are held temporarily by the video queue, the UI
    // presentation callback and the D3D11/Vulkan interop ring.  The default
    // FFmpeg pool (roughly decoder surfaces + a small reorder margin) is too
    // small for that ownership model and results in "Static surface pool
    // size exceeded" after the first few frames.
    impl_->ctx->extra_hw_frames = 32;
    impl_->ctx->time_base = {stream.codec.tbNum, stream.codec.tbDen};
    impl_->ctx->pkt_timebase = impl_->ctx->time_base;
    impl_->ctx->framerate = {stream.codec.fpsNum, stream.codec.fpsDen};
    impl_->startTime = stream.startTime;
    if (stream.codec.extradata() && stream.codec.extradataSize() > 0) {
        impl_->ctx->extradata = static_cast<uint8_t*>(
            av_mallocz(stream.codec.extradataSize() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!impl_->ctx->extradata) { impl_->close(); return -8; }
        std::memcpy(impl_->ctx->extradata, stream.codec.extradata(),
                    stream.codec.extradataSize());
        impl_->ctx->extradata_size = stream.codec.extradataSize();
    }

    const AVPixelFormat softwareFormat = stream.codec.bitDepth >= 10
        ? AV_PIX_FMT_P010 : AV_PIX_FMT_NV12;
    AVBufferRef* framesRef = av_hwframe_ctx_alloc(impl_->hwDeviceCtx);
    if (!framesRef) { impl_->close(); return -9; }
    auto* framesContext = reinterpret_cast<AVHWFramesContext*>(framesRef->data);
    framesContext->format = AV_PIX_FMT_D3D11;
    framesContext->sw_format = softwareFormat;
    framesContext->width = stream.codec.width;
    framesContext->height = stream.codec.height;
    // Keep a fixed array texture (required by D3D11VA), but make it large
    // enough for decoder reordering plus queued/in-flight presentation frames.
    framesContext->initial_pool_size = 48;
    auto* framesD3D11 = reinterpret_cast<AVD3D11VAFramesContext*>(framesContext->hwctx);
    framesD3D11->BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;
    framesD3D11->MiscFlags = 0;
    if (av_hwframe_ctx_init(framesRef) < 0) {
        av_buffer_unref(&framesRef);
        impl_->close();
        return -10;
    }
    impl_->hwFramesCtx = framesRef;

    impl_->ctx->get_format = selectD3D11Format;
    impl_->ctx->hw_device_ctx = av_buffer_ref(impl_->hwDeviceCtx);
    impl_->ctx->hw_frames_ctx = av_buffer_ref(impl_->hwFramesCtx);
    const int result = avcodec_open2(impl_->ctx, impl_->codec, nullptr);
    if (result < 0) { impl_->close(); return result; }
    impl_->pixelFormat = AV_PIX_FMT_D3D11;
    impl_->open = true;
    LOG_INFO("D3D11Decoder: opened {} ({})", impl_->codec->name,
             softwareFormat == AV_PIX_FMT_P010 ? "p010" : "nv12");
    return 0;
}

void D3D11Decoder::close() { impl_->close(); }
bool D3D11Decoder::isOpen() const { return impl_->open; }

int D3D11Decoder::sendPacket(std::shared_ptr<const Packet> packet) {
    if (!impl_->open) return -1;
    if (!packet || packet->empty()) return avcodec_send_packet(impl_->ctx, nullptr);
    AVPacket avPacket{};
    avPacket.data = const_cast<uint8_t*>(packet->data());
    avPacket.size = packet->size();
    AVRational packetTimeBase{packet->timeBaseNum, packet->timeBaseDen};
    if (packetTimeBase.num <= 0 || packetTimeBase.den <= 0)
        packetTimeBase = impl_->ctx->pkt_timebase;
    avPacket.pts = packet->hasPts
        ? av_rescale_q(packet->pts, packetTimeBase, impl_->ctx->pkt_timebase)
        : AV_NOPTS_VALUE;
    avPacket.dts = packet->hasDts
        ? av_rescale_q(packet->dts, packetTimeBase, impl_->ctx->pkt_timebase)
        : AV_NOPTS_VALUE;
    avPacket.duration = av_rescale_q(packet->duration, packetTimeBase,
                                     impl_->ctx->pkt_timebase);
    avPacket.pos = packet->filePos;
    if (packet->keyframe) avPacket.flags |= AV_PKT_FLAG_KEY;
    return avcodec_send_packet(impl_->ctx, &avPacket);
}

std::shared_ptr<AVFrame> D3D11Decoder::receiveFrame() {
    if (!impl_->open) return nullptr;
    AVFrame* raw = av_frame_alloc();
    if (!raw) return nullptr;
    if (avcodec_receive_frame(impl_->ctx, raw) < 0) {
        av_frame_free(&raw);
        return nullptr;
    }
    int64_t displayPts = raw->best_effort_timestamp;
    if (displayPts == AV_NOPTS_VALUE) displayPts = raw->pts;
    raw->time_base = impl_->ctx->pkt_timebase;
    if (displayPts != AV_NOPTS_VALUE) raw->pts = displayPts - impl_->startTime;
    return std::shared_ptr<AVFrame>(raw, avframeDeleter);
}

void D3D11Decoder::flush() {
    if (impl_->ctx) avcodec_flush_buffers(impl_->ctx);
}

DecoderBackend D3D11Decoder::backend() const { return DecoderBackend::D3D11; }
bool D3D11Decoder::isHardware() const { return true; }
int D3D11Decoder::outputPixelFormat() const { return impl_->pixelFormat; }

} // namespace heisenberg::decoder
