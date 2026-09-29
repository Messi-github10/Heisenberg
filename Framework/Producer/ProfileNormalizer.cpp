#include "ProfileNormalizer.hpp"

#include <Platform/D3D11/D3D11Context.hpp>
#include <Platform/Software/SoftwareContext.hpp>
#include <MultiMedia/Video/Renderer/ColorSpaceUtils.hpp>
#include <MultiMedia/Video/Renderer/RenderEngine.hpp>
#include <Utiles/Logger.hpp>

#include <d3d11.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
#include <libplacebo/d3d11.h>
#include <libplacebo/gpu.h>
#include <libplacebo/log.h>
#include <libplacebo/renderer.h>
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

void setError(std::string* error, const std::string& message) {
    if (error) *error = message;
}

pl_color_space profileColor(const Profile& profile) {
    pl_color_space color{};
    switch (profile.primaries()) {
        case ColorPrimaries::Bt709:
            color.primaries = PL_COLOR_PRIM_BT_709;
            break;
        case ColorPrimaries::Bt2020:
            color.primaries = PL_COLOR_PRIM_BT_2020;
            break;
        case ColorPrimaries::DisplayP3:
            color.primaries = PL_COLOR_PRIM_DISPLAY_P3;
            break;
    }
    switch (profile.transfer()) {
        case ColorTransfer::Bt1886:
            color.transfer = PL_COLOR_TRC_BT_1886;
            break;
        case ColorTransfer::Srgb:
            color.transfer = PL_COLOR_TRC_SRGB;
            break;
        case ColorTransfer::Pq:
            color.transfer = PL_COLOR_TRC_PQ;
            break;
        case ColorTransfer::Hlg:
            color.transfer = PL_COLOR_TRC_HLG;
            break;
        case ColorTransfer::Linear:
            color.transfer = PL_COLOR_TRC_LINEAR;
            break;
    }
    pl_color_space_infer(&color);
    return color;
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

AVPixelFormat avFormat(WorkingFormat format) {
    switch (format) {
        case WorkingFormat::Rgba8: return AV_PIX_FMT_RGBA;
        case WorkingFormat::Rgba16f: return AV_PIX_FMT_RGBAF16;
        case WorkingFormat::Rgba32f: return AV_PIX_FMT_RGBAF32;
    }
    return AV_PIX_FMT_RGBAF16;
}

std::shared_ptr<AVFrame> transferToSoftware(const AVFrame* source,
                                            std::string* error) {
    if (!source) {
        setError(error, "Hardware frame is missing");
        return {};
    }
    if (source->format != AV_PIX_FMT_D3D11) {
        AVFrame* clone = av_frame_clone(source);
        if (!clone) {
            setError(error, "Failed to clone source frame");
            return {};
        }
        return std::shared_ptr<AVFrame>(clone, avframeDeleter);
    }

    renderer::D3D11Context::instance().createDevice();
    AVFrame* software = av_frame_alloc();
    if (!software) {
        setError(error, "Failed to allocate software frame");
        return {};
    }
    software->format = AV_PIX_FMT_NV12;
    if (av_hwframe_transfer_data(software, source, 0) < 0) {
        av_frame_free(&software);
        setError(error, "Failed to transfer D3D11 frame to system memory");
        return {};
    }
    if (av_frame_copy_props(software, source) < 0) {
        av_frame_free(&software);
        setError(error, "Failed to copy hardware frame properties");
        return {};
    }
    return std::shared_ptr<AVFrame>(software, avframeDeleter);
}

struct WrappedSource {
    pl_frame frame{};
    pl_tex planes[2]{};

    ~WrappedSource() {
        // textures are owned by the caller via destroy()
    }

    void destroy(pl_gpu gpu) {
        for (pl_tex& plane : planes) {
            if (plane) pl_tex_destroy(gpu, &plane);
            plane = nullptr;
        }
    }
};

bool wrapHardwareSource(pl_gpu gpu, const AVFrame* source, WrappedSource& wrapped) {
    if (!gpu || !source || source->format != AV_PIX_FMT_D3D11 || !source->data[0]) {
        return false;
    }
    auto* texture = reinterpret_cast<ID3D11Texture2D*>(source->data[0]);
    const int arraySlice = static_cast<int>(
        reinterpret_cast<intptr_t>(source->data[1]));
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if (desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        pl_d3d11_wrap_params wrap{};
        wrap.tex = texture;
        wrap.array_slice = arraySlice;
        wrapped.planes[0] = pl_d3d11_wrap(gpu, &wrap);
        if (!wrapped.planes[0]) return false;
        wrapped.frame.num_planes = 1;
        wrapped.frame.planes[0].texture = wrapped.planes[0];
        wrapped.frame.planes[0].components = 4;
        wrapped.frame.planes[0].component_mapping[0] = 0;
        wrapped.frame.planes[0].component_mapping[1] = 1;
        wrapped.frame.planes[0].component_mapping[2] = 2;
        wrapped.frame.planes[0].component_mapping[3] = 3;
        wrapped.frame.repr.sys = PL_COLOR_SYSTEM_RGB;
        wrapped.frame.repr.levels = PL_COLOR_LEVELS_FULL;
        wrapped.frame.repr.alpha = PL_ALPHA_INDEPENDENT;
        wrapped.frame.color = renderer::colorSpaceFromAvFrame(source);
        wrapped.frame.crop = {0, 0,
                              static_cast<float>(source->width),
                              static_cast<float>(source->height)};
        return true;
    }

    bool is10Bit = desc.Format == DXGI_FORMAT_P010
        || desc.Format == DXGI_FORMAT_P016;
    if (source->hw_frames_ctx) {
        auto* framesContext = reinterpret_cast<AVHWFramesContext*>(
            source->hw_frames_ctx->data);
        is10Bit = framesContext && framesContext->sw_format == AV_PIX_FMT_P010;
    }

    pl_d3d11_wrap_params yParams{};
    yParams.tex = texture;
    yParams.array_slice = arraySlice;
    yParams.fmt = is10Bit ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
    yParams.w = source->width;
    yParams.h = source->height;
    wrapped.planes[0] = pl_d3d11_wrap(gpu, &yParams);
    if (!wrapped.planes[0]) return false;

    pl_d3d11_wrap_params uvParams{};
    uvParams.tex = texture;
    uvParams.array_slice = arraySlice;
    uvParams.fmt = is10Bit ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;
    uvParams.w = (source->width + 1) / 2;
    uvParams.h = (source->height + 1) / 2;
    wrapped.planes[1] = pl_d3d11_wrap(gpu, &uvParams);
    if (!wrapped.planes[1]) {
        wrapped.destroy(gpu);
        return false;
    }

    wrapped.frame.num_planes = 2;
    wrapped.frame.planes[0].texture = wrapped.planes[0];
    wrapped.frame.planes[0].components = 1;
    wrapped.frame.planes[0].component_mapping[0] = 0;
    wrapped.frame.planes[1].texture = wrapped.planes[1];
    wrapped.frame.planes[1].components = 2;
    wrapped.frame.planes[1].component_mapping[0] = 1;
    wrapped.frame.planes[1].component_mapping[1] = 2;
    wrapped.frame.color = renderer::colorSpaceFromAvFrame(source);
    wrapped.frame.repr.sys = renderer::colorSystemFromAvFrame(source);
    wrapped.frame.repr.levels = source->color_range == AVCOL_RANGE_JPEG
        ? PL_COLOR_LEVELS_FULL : PL_COLOR_LEVELS_LIMITED;
    wrapped.frame.repr.alpha = PL_ALPHA_NONE;
    if (is10Bit) {
        wrapped.frame.repr.bits.sample_depth = 16;
        wrapped.frame.repr.bits.color_depth = 10;
        wrapped.frame.repr.bits.bit_shift = 6;
    } else {
        wrapped.frame.repr.bits.sample_depth = 8;
        wrapped.frame.repr.bits.color_depth = 8;
        wrapped.frame.repr.bits.bit_shift = 0;
    }
    pl_frame_set_chroma_location(&wrapped.frame, PL_CHROMA_LEFT);
    wrapped.frame.crop = {0, 0,
                          static_cast<float>(source->width),
                          static_cast<float>(source->height)};
    return true;
}

void stampFrame(AVFrame* out, const AVFrame* source, const Profile& profile) {
    if (!out) return;
    if (source) {
        out->pts = source->pts;
        out->time_base = source->time_base;
        out->duration = source->duration;
    }
    out->sample_aspect_ratio = {
        profile.sampleAspect().num,
        profile.sampleAspect().den
    };
    out->color_range = profile.range() == ColorRange::Full
        ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
    out->color_primaries = avPrimaries(profile.primaries());
    out->color_trc = avTransfer(profile.transfer());
    out->colorspace = AVCOL_SPC_RGB;
}

std::shared_ptr<AVFrame> makeHardwareCanvas(ID3D11Texture2D* sourceTexture,
                                            const AVFrame* source,
                                            const Profile& profile,
                                            std::string* error) {
    if (!sourceTexture) {
        setError(error, "Profile canvas texture is missing");
        return {};
    }

    ID3D11Device* device = nullptr;
    sourceTexture->GetDevice(&device);
    if (!device) {
        setError(error, "Failed to query D3D11 device from canvas");
        return {};
    }
    ID3D11Texture2D* copy = nullptr;
    D3D11_TEXTURE2D_DESC desc{};
    sourceTexture->GetDesc(&desc);
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;
    desc.Usage = D3D11_USAGE_DEFAULT;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &copy))) {
        device->Release();
        setError(error, "Failed to allocate hardware Profile canvas");
        return {};
    }
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    device->Release();
    if (!context) {
        copy->Release();
        setError(error, "Failed to query D3D11 context");
        return {};
    }
    context->CopyResource(copy, sourceTexture);
    context->Release();

    AVFrame* out = av_frame_alloc();
    if (!out) {
        copy->Release();
        setError(error, "Failed to allocate hardware canvas frame");
        return {};
    }
    out->format = AV_PIX_FMT_D3D11;
    out->width = profile.width();
    out->height = profile.height();
    stampFrame(out, source, profile);
    out->data[0] = reinterpret_cast<uint8_t*>(copy);
    out->data[1] = nullptr;
    out->buf[0] = av_buffer_create(
        reinterpret_cast<uint8_t*>(copy),
        sizeof(ID3D11Texture2D*),
        releaseD3D11Texture,
        copy,
        0);
    if (!out->buf[0]) {
        copy->Release();
        av_frame_free(&out);
        setError(error, "Failed to attach hardware canvas buffer");
        return {};
    }
    return std::shared_ptr<AVFrame>(out, avframeDeleter);
}

pl_rect2df letterbox(int srcWidth, int srcHeight, int dstWidth, int dstHeight) {
    const float srcAspect = static_cast<float>(srcWidth) /
                            static_cast<float>(std::max(srcHeight, 1));
    const float dstAspect = static_cast<float>(dstWidth) /
                            static_cast<float>(std::max(dstHeight, 1));
    float cropW = static_cast<float>(dstWidth);
    float cropH = static_cast<float>(dstHeight);
    float cropX = 0.0f;
    float cropY = 0.0f;
    if (srcAspect > dstAspect) {
        cropH = cropW / srcAspect;
        cropY = (static_cast<float>(dstHeight) - cropH) * 0.5f;
    } else {
        cropW = cropH * srcAspect;
        cropX = (static_cast<float>(dstWidth) - cropW) * 0.5f;
    }
    return {cropX, cropY, cropX + cropW, cropY + cropH};
}

} // namespace

struct ProfileNormalizer::Impl {
    Profile profile;
    pl_log log = nullptr;
    pl_d3d11 d3d = nullptr;
    pl_gpu gpu = nullptr;
    pl_tex canvas = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    ID3D11Texture2D* canvasTexture = nullptr;
    ID3D11Texture2D* stagingTexture = nullptr;
    ID3D11RenderTargetView* canvasRtv = nullptr;
    std::unique_ptr<renderer::SoftwareContext> software;
    std::unique_ptr<renderer::RenderEngine> render;
    bool ready = false;

    void destroyTarget() {
        if (gpu && canvas) pl_tex_destroy(gpu, &canvas);
        canvas = nullptr;
        if (canvasRtv) {
            canvasRtv->Release();
            canvasRtv = nullptr;
        }
        if (stagingTexture) {
            stagingTexture->Release();
            stagingTexture = nullptr;
        }
        if (canvasTexture) {
            canvasTexture->Release();
            canvasTexture = nullptr;
        }
    }

    void shutdown() {
        destroyTarget();
        if (software) {
            software->shutdown();
            software.reset();
        }
        render.reset();
        if (d3d) pl_d3d11_destroy(&d3d);
        if (log) pl_log_destroy(&log);
        gpu = nullptr;
        device = nullptr;
        context = nullptr;
        ready = false;
    }

    bool createTexture(ID3D11Texture2D** out,
                       D3D11_USAGE usage,
                       UINT bindFlags,
                       UINT cpuAccess) const {
        if (!device || !out) return false;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(profile.width());
        desc.Height = static_cast<UINT>(profile.height());
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = usage;
        desc.BindFlags = bindFlags;
        desc.CPUAccessFlags = cpuAccess;
        return SUCCEEDED(device->CreateTexture2D(&desc, nullptr, out));
    }
};

ProfileNormalizer::ProfileNormalizer()
    : impl_(std::make_unique<Impl>()) {}

ProfileNormalizer::~ProfileNormalizer() {
    shutdown();
}

bool ProfileNormalizer::isReady() const {
    return impl_ && impl_->ready;
}

void ProfileNormalizer::shutdown() {
    if (impl_) impl_->shutdown();
}

bool ProfileNormalizer::initialize(const Profile& profile, std::string* error) {
    shutdown();
    impl_->profile = profile;

    auto& d3d11 = renderer::D3D11Context::instance();
    d3d11.createDevice();
    impl_->device = d3d11.device();
    impl_->context = d3d11.context();
    if (!impl_->device || !impl_->context) {
        setError(error, "Failed to create D3D11 device for Profile canvas");
        shutdown();
        return false;
    }

    static const auto logCallback = [](void*, enum pl_log_level level, const char* msg) {
        switch (level) {
            case PL_LOG_FATAL: LOG_CRITICAL("[libplacebo] {}", msg); break;
            case PL_LOG_ERR: LOG_ERROR("[libplacebo] {}", msg); break;
            case PL_LOG_WARN: LOG_WARN("[libplacebo] {}", msg); break;
            default: break;
        }
    };
    pl_log_params logParams{};
    logParams.log_cb = logCallback;
    logParams.log_level = PL_LOG_WARN;
    impl_->log = pl_log_create(PL_API_VER, &logParams);
    if (!impl_->log) {
        setError(error, "Failed to create libplacebo log");
        return false;
    }

    pl_d3d11_params d3dParams = pl_d3d11_default_params;
    d3dParams.device = impl_->device;
    d3dParams.allow_software = false;
    impl_->d3d = pl_d3d11_create(impl_->log, &d3dParams);
    if (!impl_->d3d || !impl_->d3d->gpu) {
        setError(error, "Failed to create libplacebo D3D11 GPU");
        shutdown();
        return false;
    }
    impl_->gpu = impl_->d3d->gpu;

    if (!impl_->createTexture(&impl_->canvasTexture,
                              D3D11_USAGE_DEFAULT,
                              D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
                              0)) {
        setError(error, "Failed to create Profile working texture");
        shutdown();
        return false;
    }
    if (!impl_->createTexture(&impl_->stagingTexture,
                              D3D11_USAGE_STAGING,
                              0,
                              D3D11_CPU_ACCESS_READ)) {
        setError(error, "Failed to create Profile staging texture");
        shutdown();
        return false;
    }
    if (FAILED(impl_->device->CreateRenderTargetView(
            impl_->canvasTexture, nullptr, &impl_->canvasRtv))) {
        setError(error, "Failed to create Profile canvas render target");
        shutdown();
        return false;
    }

    pl_d3d11_wrap_params wrap{};
    wrap.tex = impl_->canvasTexture;
    impl_->canvas = pl_d3d11_wrap(impl_->gpu, &wrap);
    if (!impl_->canvas) {
        setError(error, "Failed to wrap Profile working texture");
        shutdown();
        return false;
    }

    try {
        impl_->software = std::make_unique<renderer::SoftwareContext>(impl_->gpu);
        impl_->render = std::make_unique<renderer::RenderEngine>(impl_->gpu);
    } catch (const std::exception& exception) {
        setError(error, exception.what());
        shutdown();
        return false;
    }

    impl_->ready = true;
    LOG_INFO("ProfileNormalizer: {}x{} rgba16f on D3D11",
             profile.width(), profile.height());
    return true;
}

std::shared_ptr<AVFrame> ProfileNormalizer::normalize(const AVFrame* source,
                                                      std::string* error) {
    if (!isReady() || !source) {
        setError(error, "ProfileNormalizer is not ready");
        return {};
    }

    WrappedSource hardware;
    const pl_frame* uploaded = nullptr;
    std::shared_ptr<AVFrame> software;
    const bool hardwareSource = source->format == AV_PIX_FMT_D3D11;
    if (hardwareSource) {
        if (!wrapHardwareSource(impl_->gpu, source, hardware)) {
            setError(error, "Failed to wrap D3D11 source frame");
            return {};
        }
        uploaded = &hardware.frame;
    } else {
        software = transferToSoftware(source, error);
        if (!software || !software->data[0]) return {};
        uploaded = impl_->software->uploadAvFrame(software.get());
        if (!uploaded) {
            setError(error, "Failed to upload source frame");
            return {};
        }
    }

    pl_frame target = {};
    target.num_planes = 1;
    target.planes[0].texture = impl_->canvas;
    target.planes[0].components = 4;
    target.planes[0].component_mapping[0] = 0;
    target.planes[0].component_mapping[1] = 1;
    target.planes[0].component_mapping[2] = 2;
    target.planes[0].component_mapping[3] = 3;
    target.repr.sys = PL_COLOR_SYSTEM_RGB;
    target.repr.levels = impl_->profile.range() == ColorRange::Full
        ? PL_COLOR_LEVELS_PC : PL_COLOR_LEVELS_TV;
    target.repr.alpha = PL_ALPHA_INDEPENDENT;
    target.color = profileColor(impl_->profile);
    target.crop = letterbox(source->width, source->height,
                            impl_->profile.width(), impl_->profile.height());

    // Wrapped D3D11 textures do not advertise blit_dst. Letterbox bars are
    // cleared with an RTV so libplacebo does not have to blit-clear the canvas.
    if (impl_->context && impl_->canvasRtv) {
        const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        impl_->context->ClearRenderTargetView(impl_->canvasRtv, black);
    }

    pl_render_params params = pl_render_default_params;
    params.skip_target_clearing = true;
    params.background_transparency = 0.0f;
    params.corner_rounding = 0.0f;
    const bool rendered = impl_->render->render(uploaded, &target, &params);
    hardware.destroy(impl_->gpu);
    if (!rendered) {
        setError(error, "Failed to render source frame into the Profile canvas");
        return {};
    }
    if (impl_->context) impl_->context->Flush();

    if (hardwareSource) {
        return makeHardwareCanvas(impl_->canvasTexture, source,
                                  impl_->profile, error);
    }

    impl_->context->CopyResource(impl_->stagingTexture, impl_->canvasTexture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(impl_->context->Map(impl_->stagingTexture, 0,
                                   D3D11_MAP_READ, 0, &mapped))) {
        setError(error, "Failed to map Profile canvas");
        return {};
    }

    AVFrame* out = av_frame_alloc();
    if (!out) {
        impl_->context->Unmap(impl_->stagingTexture, 0);
        setError(error, "Failed to allocate normalized frame");
        return {};
    }
    out->format = avFormat(impl_->profile.workingFormat());
    out->width = impl_->profile.width();
    out->height = impl_->profile.height();
    stampFrame(out, software ? software.get() : source, impl_->profile);
    if (av_frame_get_buffer(out, 32) < 0) {
        impl_->context->Unmap(impl_->stagingTexture, 0);
        av_frame_free(&out);
        setError(error, "Failed to allocate normalized frame buffer");
        return {};
    }

    const size_t packedTexel = 8;
    const size_t srcPitch = mapped.RowPitch;
    for (int y = 0; y < out->height; ++y) {
        const uint8_t* src = static_cast<const uint8_t*>(mapped.pData)
            + static_cast<size_t>(y) * srcPitch;
        uint8_t* dst = out->data[0] + static_cast<size_t>(y) * out->linesize[0];
        if (srcPitch >= static_cast<size_t>(out->width) * packedTexel) {
            std::memcpy(dst, src, static_cast<size_t>(out->width) * packedTexel);
        }
    }
    impl_->context->Unmap(impl_->stagingTexture, 0);
    return std::shared_ptr<AVFrame>(out, avframeDeleter);
}

} // namespace heisenberg
