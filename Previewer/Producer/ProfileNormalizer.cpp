#include "ProfileNormalizer.hpp"

#include <Platform/D3D11/D3D11Context.hpp>
#include <Platform/Software/SoftwareContext.hpp>
#include <Video/Renderer/ColorSpaceUtils.hpp>
#include <Video/Renderer/RenderEngine.hpp>
#include <Utiles/Logger.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include <volk.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
#include <libplacebo/gpu.h>
#include <libplacebo/log.h>
#include <libplacebo/renderer.h>
#include <libplacebo/vulkan.h>
}

namespace heisenberg {
namespace {

void avframeDeleter(AVFrame* frame) {
    av_frame_free(&frame);
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

const char* formatName(WorkingFormat format) {
    switch (format) {
        case WorkingFormat::Rgba8: return "rgba8";
        case WorkingFormat::Rgba16f: return "rgba16f";
        case WorkingFormat::Rgba32f: return "rgba32f";
    }
    return "rgba16f";
}

AVPixelFormat avFormat(WorkingFormat format) {
    switch (format) {
        case WorkingFormat::Rgba8: return AV_PIX_FMT_RGBA;
        case WorkingFormat::Rgba16f: return AV_PIX_FMT_RGBAF16;
        case WorkingFormat::Rgba32f: return AV_PIX_FMT_RGBAF32;
    }
    return AV_PIX_FMT_RGBAF16;
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
    pl_vulkan vulkan = nullptr;
    pl_gpu gpu = nullptr;
    pl_tex target = nullptr;
    std::unique_ptr<renderer::SoftwareContext> software;
    std::unique_ptr<renderer::RenderEngine> render;
    bool ready = false;

    void destroyTarget() {
        if (gpu && target) pl_tex_destroy(gpu, &target);
        target = nullptr;
    }

    void shutdown() {
        destroyTarget();
        if (software) {
            software->shutdown();
            software.reset();
        }
        render.reset();
        if (vulkan) pl_vulkan_destroy(&vulkan);
        if (log) pl_log_destroy(&log);
        gpu = nullptr;
        ready = false;
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

    if (volkInitialize() != VK_SUCCESS) {
        setError(error, "Failed to load the Vulkan loader");
        return false;
    }
    renderer::D3D11Context::instance().createDevice();

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

    pl_vulkan_params vulkanParams = pl_vulkan_default_params;
    vulkanParams.allow_software = true;
    impl_->vulkan = pl_vulkan_create(impl_->log, &vulkanParams);
    if (!impl_->vulkan || !impl_->vulkan->gpu) {
        setError(error, "Failed to create libplacebo Vulkan GPU");
        shutdown();
        return false;
    }
    impl_->gpu = impl_->vulkan->gpu;

    const char* name = formatName(profile.workingFormat());
    pl_fmt format = nullptr;
    if (profile.workingFormat() == WorkingFormat::Rgba16f) {
        format = pl_find_named_fmt(impl_->gpu, "rgba16hf");
        if (!format) format = pl_find_named_fmt(impl_->gpu, name);
        if (format && format->texel_size != 8) {
            pl_fmt half = pl_find_named_fmt(impl_->gpu, "rgba16hf");
            if (half && half->texel_size == 8) format = half;
        }
    } else {
        format = pl_find_named_fmt(impl_->gpu, name);
    }
    if (!format) {
        setError(error, std::string("GPU does not support working format ") + name);
        shutdown();
        return false;
    }

    pl_tex_params tex{};
    tex.w = profile.width();
    tex.h = profile.height();
    tex.format = format;
    tex.renderable = true;
    tex.blit_dst = true;
    tex.host_readable = true;
    tex.storable = true;
    impl_->target = pl_tex_create(impl_->gpu, &tex);
    if (!impl_->target) {
        setError(error, "Failed to create Profile working texture");
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
    LOG_INFO("ProfileNormalizer: {}x{} {}",
             profile.width(), profile.height(), name);
    return true;
}

std::shared_ptr<AVFrame> ProfileNormalizer::normalize(const AVFrame* source,
                                                      std::string* error) {
    if (!isReady() || !source) {
        setError(error, "ProfileNormalizer is not ready");
        return {};
    }

    std::shared_ptr<AVFrame> software = transferToSoftware(source, error);
    if (!software || !software->data[0]) return {};

    const pl_frame* uploaded = impl_->software->uploadAvFrame(software.get());
    if (!uploaded) {
        setError(error, "Failed to upload source frame");
        return {};
    }

    pl_frame target = {};
    target.num_planes = 1;
    target.planes[0].texture = impl_->target;
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
    target.crop = letterbox(software->width, software->height,
                            impl_->profile.width(), impl_->profile.height());

    pl_render_params params = pl_render_default_params;
    params.skip_target_clearing = false;
    params.background_transparency = 0.0f;
    params.corner_rounding = 0.0f;
    if (!impl_->render->render(uploaded, &target, &params)) {
        setError(error, "Failed to render source frame into the Profile canvas");
        return {};
    }

    const pl_fmt format = impl_->target->params.format;
    const size_t texelSize = format && format->texel_size ? format->texel_size : 8;
    const size_t texelAlign = format && format->texel_align ? format->texel_align : 1;
    size_t rowPitch = static_cast<size_t>(impl_->profile.width()) * texelSize;
    if (texelAlign > 1) {
        rowPitch = (rowPitch + texelAlign - 1) / texelAlign * texelAlign;
    }

    std::vector<uint8_t> staging(rowPitch * static_cast<size_t>(impl_->profile.height()));
    pl_tex_transfer_params download{};
    download.tex = impl_->target;
    download.ptr = staging.data();
    download.row_pitch = rowPitch;
    if (!pl_tex_download(impl_->gpu, &download)) {
        setError(error, "Failed to download normalized frame");
        return {};
    }

    AVFrame* out = av_frame_alloc();
    if (!out) {
        setError(error, "Failed to allocate normalized frame");
        return {};
    }
    out->format = avFormat(impl_->profile.workingFormat());
    out->width = impl_->profile.width();
    out->height = impl_->profile.height();
    out->color_range = impl_->profile.range() == ColorRange::Full
        ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
    out->color_primaries = avPrimaries(impl_->profile.primaries());
    out->color_trc = avTransfer(impl_->profile.transfer());
    out->colorspace = AVCOL_SPC_RGB;
    if (av_frame_get_buffer(out, 32) < 0) {
        av_frame_free(&out);
        setError(error, "Failed to allocate normalized frame buffer");
        return {};
    }

    const int packedStride = out->linesize[0];
    const size_t packedTexel = 8;
    const bool gpuIsFloat32 = texelSize >= 16;
    for (int y = 0; y < out->height; ++y) {
        const uint8_t* src = staging.data() + static_cast<size_t>(y) * rowPitch;
        uint8_t* dst = out->data[0] + static_cast<size_t>(y) * packedStride;
        if (!gpuIsFloat32 && texelSize == packedTexel) {
            std::memcpy(dst, src, static_cast<size_t>(out->width) * packedTexel);
            continue;
        }
        for (int x = 0; x < out->width; ++x) {
            const float* pixel = reinterpret_cast<const float*>(
                src + static_cast<size_t>(x) * texelSize);
            uint16_t* half = reinterpret_cast<uint16_t*>(
                dst + static_cast<size_t>(x) * packedTexel);
            for (int c = 0; c < 4; ++c) {
                half[c] = floatToHalf(pixel[c]);
            }
        }
    }

    out->pts = software->pts;
    out->time_base = software->time_base;
    out->duration = software->duration;
    out->sample_aspect_ratio = {
        impl_->profile.sampleAspect().num,
        impl_->profile.sampleAspect().den
    };
    return std::shared_ptr<AVFrame>(out, avframeDeleter);
}

} // namespace heisenberg
