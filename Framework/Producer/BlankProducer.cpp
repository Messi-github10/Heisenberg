#include "BlankProducer.hpp"

#include <Common/AudioFrame.hpp>
#include <Common/AudioSpec.hpp>
#include <Platform/D3D11/D3D11Context.hpp>
#include <Utiles/Logger.hpp>

#include <d3d11.h>

#include <algorithm>
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

void fillWhite(AVFrame* frame, WorkingFormat format) {
    if (!frame || !frame->data[0]) return;
    const int width = frame->width;
    const int height = frame->height;
    const int stride = frame->linesize[0];
    switch (format) {
        case WorkingFormat::Rgba8:
            for (int y = 0; y < height; ++y) {
                uint8_t* row = frame->data[0] + static_cast<size_t>(y) * stride;
                std::memset(row, 255, static_cast<size_t>(width) * 4);
            }
            break;
        case WorkingFormat::Rgba16f: {
            const uint16_t white = floatToHalf(1.0f);
            for (int y = 0; y < height; ++y) {
                auto* row = reinterpret_cast<uint16_t*>(
                    frame->data[0] + static_cast<size_t>(y) * stride);
                for (int x = 0; x < width * 4; ++x) row[x] = white;
            }
            break;
        }
        case WorkingFormat::Rgba32f:
            for (int y = 0; y < height; ++y) {
                auto* row = reinterpret_cast<float*>(
                    frame->data[0] + static_cast<size_t>(y) * stride);
                for (int x = 0; x < width * 4; ++x) row[x] = 1.0f;
            }
            break;
    }
}

std::shared_ptr<AVFrame> makeWhiteCpuCanvas(const Profile& profile) {
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
    fillWhite(frame, profile.workingFormat());
    return std::shared_ptr<AVFrame>(frame, avframeDeleter);
}

std::shared_ptr<AVFrame> makeWhiteGpuCanvas(const Profile& profile) {
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
    const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    context->ClearRenderTargetView(rtv, white);
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

} // namespace

struct BlankProducer::Impl {
    Profile profile = Profile::hd1080p24();
    std::string resource = kBlankResource;
    bool hardwareDecode = false;
    int64_t length = 0;
    int64_t position = 0;
    std::shared_ptr<AVFrame> white;

    int64_t clampPosition(int64_t value) const {
        if (length <= 0) return 0;
        return std::clamp(value, int64_t{0}, length - 1);
    }

    bool ensureWhite() {
        if (white) return true;
        if (hardwareDecode) {
            white = makeWhiteGpuCanvas(profile);
            if (white) {
                LOG_INFO("BlankProducer: {}x{} white canvas on D3D11",
                         profile.width(), profile.height());
                return true;
            }
            LOG_WARN("BlankProducer: GPU canvas failed, using CPU white");
        }
        white = makeWhiteCpuCanvas(profile);
        if (white) {
            LOG_INFO("BlankProducer: {}x{} white canvas on CPU",
                     profile.width(), profile.height());
        }
        return static_cast<bool>(white);
    }
};

BlankProducer::BlankProducer(Profile profile, int64_t length)
    : impl_(std::make_unique<Impl>()) {
    impl_->profile = std::move(profile);
    ensureLength(length);
}

BlankProducer::~BlankProducer() = default;

void BlankProducer::setHardwareDecode(bool enabled) {
    if (impl_->hardwareDecode == enabled) return;
    impl_->hardwareDecode = enabled;
    impl_->white.reset();
}

void BlankProducer::ensureLength(int64_t length) {
    if (length > impl_->length) impl_->length = length;
}

const Profile& BlankProducer::profile() const {
    return impl_->profile;
}

const std::string& BlankProducer::resource() const {
    return impl_->resource;
}

int64_t BlankProducer::in() const {
    return 0;
}

int64_t BlankProducer::out() const {
    return impl_->length > 0 ? impl_->length - 1 : 0;
}

int64_t BlankProducer::length() const {
    return impl_->length;
}

int64_t BlankProducer::position() const {
    return impl_->position;
}

bool BlankProducer::seekable() const {
    return true;
}

bool BlankProducer::seek(int64_t position) {
    if (impl_->length <= 0) return false;
    impl_->position = impl_->clampPosition(position);
    return true;
}

ProducerFrame BlankProducer::getFrame(int64_t position) {
    ProducerFrame frame;
    if (impl_->length <= 0) {
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

    if (!impl_->ensureWhite()) {
        frame.eof = true;
        impl_->position = clamped;
        return frame;
    }

    AVFrame* clone = av_frame_clone(impl_->white.get());
    if (!clone) {
        frame.eof = true;
        impl_->position = clamped;
        return frame;
    }
    clone->pts = clamped;
    clone->time_base = {
        impl_->profile.timeBase().num,
        impl_->profile.timeBase().den
    };
    clone->duration = 1;
    frame.video = std::shared_ptr<AVFrame>(clone, avframeDeleter);
    frame.audio = makeSilentAudio(impl_->profile, clamped);
    impl_->position = clamped;
    return frame;
}

bool BlankProducer::isBlank() const {
    return true;
}

} // namespace heisenberg
