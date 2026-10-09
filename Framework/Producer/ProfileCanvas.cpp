#include "ProfileCanvas.hpp"

#include <Common/AudioFrame.hpp>
#include <Common/AudioSpec.hpp>
#include <Platform/D3D11/D3D11Context.hpp>
#include <Utiles/Logger.hpp>

#include <d3d11.h>

#include <cstdint>
#include <cstring>
#include <memory>

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace heisenberg {
namespace {

void avframeDeleter(AVFrame* frame) {
    av_frame_free(&frame);
}

void releaseD3D11Texture(void* opaque, uint8_t*) {
    auto* texture = static_cast<ID3D11Texture2D*>(opaque);
    if (texture) texture->Release();
}

uint16_t floatToHalf(float value) {
    union {
        float f;
        uint32_t u;
    } bits{value};
    const uint32_t sign = (bits.u >> 16) & 0x8000u;
    const int32_t exponent = static_cast<int32_t>((bits.u >> 23) & 0xFFu) - 127 + 15;
    const uint32_t mantissa = bits.u & 0x7FFFFFu;
    if (exponent <= 0) {
        if (exponent < -10) return static_cast<uint16_t>(sign);
        const uint32_t denorm = (mantissa | 0x800000u) >> (1 - exponent);
        return static_cast<uint16_t>(sign | ((denorm + 0x1000u) >> 13));
    }
    if (exponent >= 31) {
        if (mantissa) return static_cast<uint16_t>(sign | 0x7FFFu);
        return static_cast<uint16_t>(sign | 0x7C00u);
    }
    return static_cast<uint16_t>(
        sign | (static_cast<uint32_t>(exponent) << 10) | ((mantissa + 0x1000u) >> 13));
}

AVPixelFormat avFormat(WorkingFormat format) {
    switch (format) {
        case WorkingFormat::Rgba8: return AV_PIX_FMT_RGBA;
        case WorkingFormat::Rgba16f: return AV_PIX_FMT_RGBAF16;
        case WorkingFormat::Rgba32f: return AV_PIX_FMT_RGBAF32;
    }
    return AV_PIX_FMT_RGBAF16;
}

AVColorPrimaries avPrimaries(ColorPrimaries primaries) {
    switch (primaries) {
        case ColorPrimaries::Bt709: return AVCOL_PRI_BT709;
        case ColorPrimaries::Bt2020: return AVCOL_PRI_BT2020;
        case ColorPrimaries::DisplayP3: return AVCOL_PRI_SMPTE432;
    }
    return AVCOL_PRI_BT709;
}

AVColorTransferCharacteristic avTransfer(ColorTransfer transfer) {
    switch (transfer) {
        case ColorTransfer::Bt1886: return AVCOL_TRC_BT709;
        case ColorTransfer::Srgb: return AVCOL_TRC_IEC61966_2_1;
        case ColorTransfer::Pq: return AVCOL_TRC_SMPTE2084;
        case ColorTransfer::Hlg: return AVCOL_TRC_ARIB_STD_B67;
        case ColorTransfer::Linear: return AVCOL_TRC_LINEAR;
    }
    return AVCOL_TRC_BT709;
}

void stampCanvas(AVFrame* frame, const Profile& profile) {
    if (!frame) return;
    frame->color_range = profile.range() == ColorRange::Full
        ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
    frame->color_primaries = avPrimaries(profile.primaries());
    frame->color_trc = avTransfer(profile.transfer());
    frame->colorspace = AVCOL_SPC_RGB;
    frame->sample_aspect_ratio = {
        profile.sampleAspect().num,
        profile.sampleAspect().den
    };
}

void rgba(CanvasColor color, float out[4]) {
    if (color == CanvasColor::White) {
        out[0] = out[1] = out[2] = out[3] = 1.0f;
        return;
    }
    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
}

void fillRgba(AVFrame* frame, WorkingFormat format, const float color[4]) {
    if (!frame || !frame->data[0]) return;
    const int width = frame->width;
    const int height = frame->height;
    const int stride = frame->linesize[0];
    switch (format) {
        case WorkingFormat::Rgba8: {
            const uint8_t pixel[4] = {
                static_cast<uint8_t>(color[0] * 255.0f + 0.5f),
                static_cast<uint8_t>(color[1] * 255.0f + 0.5f),
                static_cast<uint8_t>(color[2] * 255.0f + 0.5f),
                static_cast<uint8_t>(color[3] * 255.0f + 0.5f),
            };
            for (int y = 0; y < height; ++y) {
                uint8_t* row = frame->data[0] + static_cast<size_t>(y) * stride;
                for (int x = 0; x < width; ++x) {
                    std::memcpy(row + static_cast<size_t>(x) * 4, pixel, 4);
                }
            }
            break;
        }
        case WorkingFormat::Rgba16f: {
            const uint16_t pixel[4] = {
                floatToHalf(color[0]),
                floatToHalf(color[1]),
                floatToHalf(color[2]),
                floatToHalf(color[3]),
            };
            for (int y = 0; y < height; ++y) {
                auto* row = reinterpret_cast<uint16_t*>(
                    frame->data[0] + static_cast<size_t>(y) * stride);
                for (int x = 0; x < width; ++x) {
                    row[x * 4 + 0] = pixel[0];
                    row[x * 4 + 1] = pixel[1];
                    row[x * 4 + 2] = pixel[2];
                    row[x * 4 + 3] = pixel[3];
                }
            }
            break;
        }
        case WorkingFormat::Rgba32f:
            for (int y = 0; y < height; ++y) {
                auto* row = reinterpret_cast<float*>(
                    frame->data[0] + static_cast<size_t>(y) * stride);
                for (int x = 0; x < width; ++x) {
                    row[x * 4 + 0] = color[0];
                    row[x * 4 + 1] = color[1];
                    row[x * 4 + 2] = color[2];
                    row[x * 4 + 3] = color[3];
                }
            }
            break;
    }
}

std::shared_ptr<AVFrame> makeCpuCanvas(const Profile& profile, CanvasColor color) {
    AVFrame* frame = av_frame_alloc();
    if (!frame) return {};
    frame->format = avFormat(profile.workingFormat());
    frame->width = profile.width();
    frame->height = profile.height();
    stampCanvas(frame, profile);
    if (av_frame_get_buffer(frame, 32) < 0) {
        av_frame_free(&frame);
        return {};
    }
    float rgbaColor[4];
    rgba(color, rgbaColor);
    fillRgba(frame, profile.workingFormat(), rgbaColor);
    return std::shared_ptr<AVFrame>(frame, avframeDeleter);
}

std::shared_ptr<AVFrame> makeGpuCanvas(const Profile& profile, CanvasColor color) {
    auto& d3d11 = renderer::D3D11Context::instance();
    d3d11.createDevice();
    ID3D11Device* device = d3d11.device();
    ID3D11DeviceContext* context = d3d11.context();
    if (!device || !context) return {};

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(profile.width());
    desc.Height = static_cast<UINT>(profile.height());
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D* texture = nullptr;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &texture))) return {};

    ID3D11RenderTargetView* rtv = nullptr;
    if (FAILED(device->CreateRenderTargetView(texture, nullptr, &rtv))) {
        texture->Release();
        return {};
    }
    float clear[4];
    rgba(color, clear);
    context->ClearRenderTargetView(rtv, clear);
    context->Flush();
    rtv->Release();

    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        texture->Release();
        return {};
    }
    frame->format = AV_PIX_FMT_D3D11;
    frame->width = profile.width();
    frame->height = profile.height();
    stampCanvas(frame, profile);
    frame->data[0] = reinterpret_cast<uint8_t*>(texture);
    frame->data[1] = nullptr;
    frame->buf[0] = av_buffer_create(
        reinterpret_cast<uint8_t*>(texture),
        sizeof(ID3D11Texture2D*),
        releaseD3D11Texture,
        texture,
        0);
    if (!frame->buf[0]) {
        texture->Release();
        av_frame_free(&frame);
        return {};
    }
    return std::shared_ptr<AVFrame>(frame, avframeDeleter);
}

std::shared_ptr<AudioFrame> makeSilentAudio(const Profile& profile, int64_t position) {
    auto audio = std::make_shared<AudioFrame>();
    AudioSpec spec;
    spec.sampleRate = profile.sampleRate();
    spec.channels = profile.channels();
    spec.layout = defaultLayout(spec.channels);
    audio->setSpec(spec);
    audio->setSamples(static_cast<int>(profile.audioSamplesForFrame(position)), true);
    audio->clear();
    audio->setPosition(profile.audioSamplePosition(position));
    return audio;
}

const char* colorName(CanvasColor color) {
    return color == CanvasColor::White ? "white" : "black";
}

} // namespace

struct ProfileCanvas::Impl {
    Profile profile = Profile::hd1080p24();
    bool hardwareDecode = false;
    std::shared_ptr<AVFrame> black;
    std::shared_ptr<AVFrame> white;

    std::shared_ptr<AVFrame>& slot(CanvasColor color) {
        return color == CanvasColor::White ? white : black;
    }

    bool ensure(CanvasColor color) {
        auto& cached = slot(color);
        if (cached) return true;
        if (hardwareDecode) {
            cached = makeGpuCanvas(profile, color);
            if (cached) {
                LOG_INFO("ProfileCanvas: {}x{} {} canvas on D3D11",
                         profile.width(), profile.height(), colorName(color));
                return true;
            }
            LOG_WARN("ProfileCanvas: GPU {} canvas failed, using CPU",
                     colorName(color));
        }
        cached = makeCpuCanvas(profile, color);
        if (cached) {
            LOG_INFO("ProfileCanvas: {}x{} {} canvas on CPU",
                     profile.width(), profile.height(), colorName(color));
        }
        return static_cast<bool>(cached);
    }

    void reset() {
        black.reset();
        white.reset();
    }
};

ProfileCanvas::ProfileCanvas(Profile profile)
    : impl_(std::make_unique<Impl>()) {
    impl_->profile = std::move(profile);
}

ProfileCanvas::~ProfileCanvas() = default;

void ProfileCanvas::setHardwareDecode(bool enabled) {
    if (impl_->hardwareDecode == enabled) return;
    impl_->hardwareDecode = enabled;
    impl_->reset();
}

void ProfileCanvas::setProfile(Profile profile) {
    impl_->profile = std::move(profile);
    impl_->reset();
}

const Profile& ProfileCanvas::profile() const {
    return impl_->profile;
}

ProducerFrame ProfileCanvas::frame(int64_t position, CanvasColor color) {
    ProducerFrame frame;
    if (!impl_->ensure(color)) {
        frame.eof = true;
        frame.position = position;
        return frame;
    }

    AVFrame* clone = av_frame_clone(impl_->slot(color).get());
    if (!clone) {
        frame.eof = true;
        frame.position = position;
        return frame;
    }
    clone->pts = position;
    clone->time_base = {
        impl_->profile.timeBase().num,
        impl_->profile.timeBase().den
    };
    clone->duration = 1;
    frame.position = position;
    frame.video = std::shared_ptr<AVFrame>(clone, avframeDeleter);
    frame.audio = makeSilentAudio(impl_->profile, position);
    return frame;
}

} // namespace heisenberg
