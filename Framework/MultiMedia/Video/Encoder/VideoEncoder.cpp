#include "VideoEncoder.hpp"
#include "EncoderInternal.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

extern "C" {
#include <libavutil/common.h>
#include <libavutil/dict.h>
#include <libavutil/pixfmt.h>
}

namespace heisenberg {
using namespace encoding_detail;
namespace {
int pixelBytes(EncodedRgbFormat format) {
    switch (format) {
    case EncodedRgbFormat::RGB24: return 3;
    case EncodedRgbFormat::RGBA8: return 4;
    case EncodedRgbFormat::RGBA16F: return 8;
    case EncodedRgbFormat::RGBA32F: return 16;
    }
    throw std::invalid_argument("unsupported RGB format");
}
double half(const uint8_t* data) {
    uint16_t bits;
    std::memcpy(&bits, data, sizeof(bits));
    const unsigned exponent = (bits >> 10) & 31;
    const unsigned fraction = bits & 1023;
    double value;
    if (exponent == 31) throw std::invalid_argument("RGB half contains NaN/Inf");
    if (exponent == 0) value = std::ldexp(static_cast<double>(fraction), -24);
    else value = std::ldexp(1.0 + fraction / 1024.0, static_cast<int>(exponent) - 15);
    return (bits & 0x8000) ? -value : value;
}
double component(const uint8_t* data, EncodedRgbFormat format, int index) {
    double result;
    if (format == EncodedRgbFormat::RGBA32F) {
        float value;
        std::memcpy(&value, data + index * 4, sizeof(value));
        result = value;
    } else if (format == EncodedRgbFormat::RGBA16F) result = half(data + index * 2);
    else result = data[index] / 255.0;
    require(std::isfinite(result), "RGB contains NaN/Inf");
    return std::clamp(result, 0.0, 1.0);
}
uint8_t quantize(double value, int low, int high) {
    return static_cast<uint8_t>(std::clamp(std::lround(value), static_cast<long>(low), static_cast<long>(high)));
}
}

struct VideoEncoder::Impl {
    CodecPtr context;
    FramePtr yuv;
    bool drained = false, failed = false;
    int64_t lastPts = -1;
    explicit Impl(const VideoEncoderSettings& settings) {
        require(settings.width > 0 && settings.height > 0 && !(settings.width & 1) && !(settings.height & 1),
                "YUV420p requires positive even dimensions");
        encoding_detail::timeBase(settings.frameRate);
        encoding_detail::timeBase(settings.sampleAspect);
        require(settings.crf >= -1 && settings.crf <= 51, "CRF must be -1 or 0..51");
        require(!settings.preset.empty(), "preset must not be empty");
        require(settings.codec == VideoEncodingCodec::H264 || settings.codec == VideoEncodingCodec::H265,
                "unsupported video codec");
        const bool hevc = settings.codec == VideoEncodingCodec::H265;
        const AVCodec* codec = avcodec_find_encoder_by_name(hevc ? "libx265" : "libx264");
        if (!codec) throw EncodingError(hevc ? "libx265 encoder unavailable" : "libx264 encoder unavailable");
        context.reset(avcodec_alloc_context3(codec));
        if (!context) throw std::bad_alloc();
        context->width = settings.width;
        context->height = settings.height;
        context->time_base = av_inv_q(settings.frameRate);
        context->framerate = settings.frameRate;
        context->sample_aspect_ratio = settings.sampleAspect;
        context->pix_fmt = AV_PIX_FMT_YUV420P;
        context->color_range = AVCOL_RANGE_MPEG;
        context->colorspace = AVCOL_SPC_BT709;
        context->color_primaries = AVCOL_PRI_BT709;
        context->color_trc = AVCOL_TRC_BT709;
        context->chroma_sample_location = AVCHROMA_LOC_CENTER;
        context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        context->max_b_frames = 3;
        context->gop_size = static_cast<int>(std::clamp<int64_t>(av_rescale_rnd(2, settings.frameRate.num,
                                                   settings.frameRate.den, AV_ROUND_UP), 1, 600));
        if (hevc) context->codec_tag = MKTAG('h', 'v', 'c', '1');
        AVDictionary* options = nullptr;
        const std::string crf = std::to_string(settings.crf < 0 ? (hevc ? 24 : 20) : settings.crf);
        try {
            check(av_dict_set(&options, "preset", settings.preset.c_str(), 0), "set preset");
            check(av_dict_set(&options, "crf", crf.c_str(), 0), "set CRF");
            // hvc1 requires parameter sets in extradata, not repeated in samples.
            if (hevc) check(av_dict_set(&options, "x265-params", "repeat-headers=0", 0), "set x265 parameters");
            check(avcodec_open2(context.get(), codec, &options), "avcodec_open2(video)");
            if (av_dict_count(options)) throw EncodingError("video encoder left unrecognized options");
        } catch (...) { av_dict_free(&options); throw; }
        av_dict_free(&options);
        yuv = frame();
        yuv->format = context->pix_fmt;
        yuv->width = context->width;
        yuv->height = context->height;
        yuv->sample_aspect_ratio = context->sample_aspect_ratio;
        yuv->color_range = context->color_range;
        yuv->colorspace = context->colorspace;
        yuv->color_primaries = context->color_primaries;
        yuv->color_trc = context->color_trc;
        yuv->chroma_location = context->chroma_sample_location;
        check(av_frame_get_buffer(yuv.get(), 32), "av_frame_get_buffer(video)");
    }
    void state() const {
        if (failed) throw EncodingError("video encoder failed; discard this instance");
        if (drained) throw EncodingError("video encoder already drained");
    }
    void convert(const EncodedRgbFrameView& view) {
        const int bytes = pixelBytes(view.format);
        require(view.data != nullptr, "RGB data is null");
        require(view.width == context->width && view.height == context->height, "RGB dimensions do not match encoder");
        const int64_t minimumRow = static_cast<int64_t>(view.width) * bytes;
        require(view.rowBytes >= minimumRow && view.rowBytes <= std::numeric_limits<std::ptrdiff_t>::max() / view.height,
                "invalid or overflowing RGB stride");
        check(av_frame_make_writable(yuv.get()), "av_frame_make_writable(video)");
        // Direct full-range R'G'B' -> limited BT.709 Y'CbCr, not a relabel.
        // Box-average the four nonlinear chroma samples, yielding center-sited
        // 4:2:0. Keep float precision until final 8-bit quantization.
        const auto* base = static_cast<const uint8_t*>(view.data);
        for (int y = 0; y < view.height; y += 2) {
            for (int x = 0; x < view.width; x += 2) {
                double cb = 0.0, cr = 0.0;
                for (int dy = 0; dy < 2; ++dy) for (int dx = 0; dx < 2; ++dx) {
                    const auto* p = base + (y + dy) * view.rowBytes + static_cast<std::ptrdiff_t>(x + dx) * bytes;
                    const double r = component(p, view.format, 0);
                    const double g = component(p, view.format, 1);
                    const double b = component(p, view.format, 2);
                    const double luma = 0.2126 * r + 0.7152 * g + 0.0722 * b;
                    yuv->data[0][(y + dy) * yuv->linesize[0] + x + dx] = quantize(16.0 + 219.0 * luma, 16, 235);
                    cb += (b - luma) / (2.0 * (1.0 - 0.0722));
                    cr += (r - luma) / (2.0 * (1.0 - 0.2126));
                }
                yuv->data[1][(y / 2) * yuv->linesize[1] + x / 2] = quantize(128.0 + 56.0 * cb, 16, 240);
                yuv->data[2][(y / 2) * yuv->linesize[2] + x / 2] = quantize(128.0 + 56.0 * cr, 16, 240);
            }
        }
    }
};

VideoEncoder::VideoEncoder(const VideoEncoderSettings& settings) : impl_(std::make_unique<Impl>(settings)) {}
VideoEncoder::~VideoEncoder() = default;
CodecParameters VideoEncoder::codecParameters() const { return CodecParameters(*impl_->context); }
AVRational VideoEncoder::timeBase() const noexcept { return impl_->context->time_base; }
EncodedPackets VideoEncoder::encode(const EncodedRgbFrameView& rgb, int64_t pts) {
    impl_->state();
    require(pts >= 0 && pts < std::numeric_limits<int64_t>::max() && pts > impl_->lastPts,
            "video PTS must be nonnegative and strictly increasing");
    impl_->convert(rgb);
    impl_->yuv->pts = pts;
    impl_->yuv->duration = 1;
    EncodedPackets packets;
    try {
        send(impl_->context.get(), impl_->yuv.get(), [&](AVPacket& packet) {
            if (packet.duration <= 0) packet.duration = 1;
            packets.emplace_back(packet, timeBase());
        });
        impl_->lastPts = pts;
    } catch (...) { impl_->failed = true; throw; }
    return packets;
}
EncodedPackets VideoEncoder::encode(const AVFrame& rgb, int64_t pts) {
    require(!rgb.hw_frames_ctx, "video input must be CPU RGB");
    EncodedRgbFormat format;
    switch (rgb.format) {
    case AV_PIX_FMT_RGBA: format = EncodedRgbFormat::RGBA8; break;
    case AV_PIX_FMT_RGB24: format = EncodedRgbFormat::RGB24; break;
    case AV_PIX_FMT_RGBAF32: format = EncodedRgbFormat::RGBA32F; break;
    case AV_PIX_FMT_RGBAF16: format = EncodedRgbFormat::RGBA16F; break;
    default: throw std::invalid_argument("video AVFrame must be RGB24/RGBA/native RGBAF32/native RGBAF16");
    }
    return encode({rgb.data[0], rgb.linesize[0], rgb.width, rgb.height, format}, pts);
}
EncodedPackets VideoEncoder::drain() {
    if (impl_->failed) throw EncodingError("video encoder failed; discard this instance");
    if (impl_->drained) return {};
    EncodedPackets packets;
    try {
        send(impl_->context.get(), nullptr, [&](AVPacket& packet) {
            if (packet.duration <= 0) packet.duration = 1;
            packets.emplace_back(packet, timeBase());
        });
        impl_->drained = true;
    } catch (...) { impl_->failed = true; throw; }
    return packets;
}
} // namespace heisenberg
