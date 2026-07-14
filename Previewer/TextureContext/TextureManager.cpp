//
// Created by NiceFold on 2026/7/14.
//

#include "TextureManager.hpp"
#include "ITexturePool.hpp"
#include "VulkanTexturePool.hpp"

#include <volk.h>
#include <libplacebo/vulkan.h>
#include <Utiles/Logger.hpp>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
}

#include <cstring>
#include <stdexcept>
#include <algorithm>

namespace heisenberg {
namespace renderer {

namespace {

struct PlaneMeta {
    int components   = 0;
    int compMapping[4] = {};
    int bytesPerComp = 1;
    int chromaShiftW = 0;
    int chromaShiftH = 0;
};

struct FormatMeta {
    AVPixelFormat avFormat  = AV_PIX_FMT_NONE;
    int           numPlanes = 0;
    PlaneMeta     planes[4];
};

// ---- 静态格式表 ----

struct StaticFormatEntry {
    AVPixelFormat avFormat;
    int8_t compPlane[4];
    int8_t bitDepth;
    int8_t chromaSW;
    int8_t chromaSH;
};

static const StaticFormatEntry kStaticFormats[] = {
    { AV_PIX_FMT_YUV420P,     {0, 1, 2, -1},  8, 1, 1 },
    { AV_PIX_FMT_NV12,        {0, 1, 1, -1},  8, 1, 1 },
    { AV_PIX_FMT_YUV420P10LE, {0, 1, 2, -1}, 10, 1, 1 },
};

static const char* getPlaneFormatName(int bytesPerComp, int components) {
    if (components < 1 || components > 2) return nullptr;
    if (bytesPerComp < 1 || bytesPerComp > 2) return nullptr;
    static const char* table[2][2] = {
        { "r8",   "rg8"   },
        { "r16",  "rg16"  },
    };
    return table[bytesPerComp - 1][components - 1];
}

static FormatMeta deriveFromStaticEntry(const StaticFormatEntry& entry) {
    FormatMeta meta;
    meta.avFormat = entry.avFormat;

    int maxPlane = -1;
    for (int c = 0; c < 4; ++c) {
        if (entry.compPlane[c] > maxPlane)
            maxPlane = entry.compPlane[c];
    }
    meta.numPlanes = maxPlane + 1;

    int bytes = (entry.bitDepth > 8) ? 2 : 1;

    for (int p = 0; p < meta.numPlanes; ++p) {
        PlaneMeta& pm = meta.planes[p];
        pm.bytesPerComp = bytes;

        int slot = 0;
        for (int c = 0; c < 4; ++c) {
            if (entry.compPlane[c] == p)
                pm.compMapping[slot++] = c;
        }
        pm.components = slot;
        for (; slot < 4; ++slot) {
            pm.compMapping[slot] = -1;
        }

        pm.chromaShiftW = (p == 0) ? 0 : entry.chromaSW;
        pm.chromaShiftH = (p == 0) ? 0 : entry.chromaSH;
    }

    return meta;
}

static FormatMeta deriveFromFFmpeg(const AVPixFmtDescriptor* desc, AVPixelFormat avfmt) {
    FormatMeta meta;
    meta.avFormat  = avfmt;
    meta.numPlanes = av_pix_fmt_count_planes(avfmt);

    for (int p = 0; p < meta.numPlanes; ++p) {
        PlaneMeta& pm = meta.planes[p];

        int bits = 8;
        for (int c = 0; c < desc->nb_components; ++c) {
            if (desc->comp[c].plane == p) {
                bits = desc->comp[c].depth;
                break;
            }
        }
        pm.bytesPerComp = (bits > 8) ? 2 : 1;

        int slot = 0;
        for (int c = 0; c < desc->nb_components; ++c) {
            if (desc->comp[c].plane == p)
                pm.compMapping[slot++] = c;
        }
        pm.components = slot;
        for (; slot < 4; ++slot) {
            pm.compMapping[slot] = -1;
        }

        pm.chromaShiftW = (p == 0) ? 0 : desc->log2_chroma_w;
        pm.chromaShiftH = (p == 0) ? 0 : desc->log2_chroma_h;
    }

    return meta;
}

static bool getFormatMeta(AVPixelFormat avfmt, FormatMeta& out) {
    // Level 1: 静态表二分查找
    int lo = 0, hi = static_cast<int>(sizeof(kStaticFormats) / sizeof(kStaticFormats[0])) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (kStaticFormats[mid].avFormat == avfmt) {
            out = deriveFromStaticEntry(kStaticFormats[mid]);
            return true;
        }
        if (kStaticFormats[mid].avFormat < avfmt)
            lo = mid + 1;
        else
            hi = mid - 1;
    }

    // Level 2: FFmpeg 回退
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(avfmt);
    if (desc) {
        out = deriveFromFFmpeg(desc, avfmt);
        return true;
    }

    LOG_ERROR("TextureManager: unsupported pixel format {} ({})",
              static_cast<int>(avfmt), av_get_pix_fmt_name(avfmt));
    return false;
}

static pl_color_repr avToPlColorRepr(const AVFrame* f) {
    pl_color_repr r = {};

    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(
        static_cast<AVPixelFormat>(f->format));

    if (desc) {
        r.bits.sample_depth = desc->comp[0].depth;
        r.bits.color_depth  = desc->comp[0].depth;
        for (int c = 1; c < desc->nb_components; ++c) {
            if (desc->comp[c].depth > r.bits.color_depth)
                r.bits.color_depth = desc->comp[c].depth;
        }
    }

    r.levels = (f->color_range == AVCOL_RANGE_JPEG)
                   ? PL_COLOR_LEVELS_PC
                   : PL_COLOR_LEVELS_TV;

    switch (f->colorspace) {
        case AVCOL_SPC_BT709:       r.sys = PL_COLOR_SYSTEM_BT_709;      break;
        case AVCOL_SPC_BT470BG:
        case AVCOL_SPC_SMPTE170M:   r.sys = PL_COLOR_SYSTEM_BT_601;      break;
        case AVCOL_SPC_BT2020_NCL:  r.sys = PL_COLOR_SYSTEM_BT_2020_NC;  break;
        case AVCOL_SPC_BT2020_CL:   r.sys = PL_COLOR_SYSTEM_BT_2020_C;   break;
        default:                    r.sys = PL_COLOR_SYSTEM_BT_709;       break;
    }

    return r;
}

static pl_color_space avToPlColor(const AVFrame* f) {
    pl_color_space c = {};

    switch (f->color_trc) {
        case AVCOL_TRC_BT709:       c.transfer = PL_COLOR_TRC_BT_1886;   break;
        case AVCOL_TRC_GAMMA22:     c.transfer = PL_COLOR_TRC_GAMMA22;   break;
        case AVCOL_TRC_GAMMA28:     c.transfer = PL_COLOR_TRC_GAMMA28;   break;
        case AVCOL_TRC_SMPTE2084:   c.transfer = PL_COLOR_TRC_PQ;        break;
        case AVCOL_TRC_ARIB_STD_B67:c.transfer = PL_COLOR_TRC_HLG;       break;
        case AVCOL_TRC_LINEAR:      c.transfer = PL_COLOR_TRC_LINEAR;    break;
        default:                    c.transfer = PL_COLOR_TRC_BT_1886;   break;
    }

    switch (f->color_primaries) {
        case AVCOL_PRI_BT709:       c.primaries = PL_COLOR_PRIM_BT_709;     break;
        case AVCOL_PRI_BT470BG:     c.primaries = PL_COLOR_PRIM_BT_601_625; break;
        case AVCOL_PRI_SMPTE170M:   c.primaries = PL_COLOR_PRIM_BT_601_525; break;
        case AVCOL_PRI_BT2020:      c.primaries = PL_COLOR_PRIM_BT_2020;    break;
        default:                    c.primaries = PL_COLOR_PRIM_BT_709;      break;
    }

    return c;
}

} // namespace (anonymous)

struct TextureManager::Impl {
    pl_gpu     gpu          = nullptr;
    vk::Device device;
    uint32_t   queueFamily  = 0;

    std::unique_ptr<ITexturePool> pool;

    // Hold semaphore
    VkSemaphore holdSemaphore = VK_NULL_HANDLE;

    // ---- 上传状态 ----
    pl_tex       uploadPlanes[4] = {};      // PL_MAX_PLANES == 4
    pl_frame     uploadFrame     = {};
    AVPixelFormat cachedUploadFormat = AV_PIX_FMT_NONE;
    int          cachedUploadWidth  = 0;
    int          cachedUploadHeight = 0;

    // ---- 目标帧 ----
    pl_frame targetFrame = {};

    // ---- 目标格式 ----
    pl_fmt    targetPlFmt   = nullptr;
    vk::Format targetVkFormat = vk::Format::eUndefined;
};

TextureManager::TextureManager(pl_gpu gpu, vk::Device device, uint32_t queueFamily)
    : impl_(std::make_unique<Impl>()) {
    impl_->gpu         = gpu;
    impl_->device      = device;
    impl_->queueFamily = queueFamily;

    // 创建 VulkanTexturePool
    impl_->pool = std::make_unique<VulkanTexturePool>(device, HEISENBERG_TEXTURE_POOL_SIZE);

    // 创建 hold semaphore
    pl_vulkan_sem_params semParams = {};
    semParams.type = VK_SEMAPHORE_TYPE_BINARY;
    impl_->holdSemaphore = pl_vulkan_sem_create(gpu, &semParams);
    if (!impl_->holdSemaphore) {
        throw std::runtime_error("TextureManager: failed to create hold semaphore");
    }

    // 查询目标格式
    impl_->targetPlFmt = pl_find_named_fmt(gpu, "rgba8");
    if (!impl_->targetPlFmt) {
        throw std::runtime_error("TextureManager: GPU does not support rgba8 format");
    }
    impl_->targetVkFormat = vk::Format::eR8G8B8A8Unorm;

    LOG_INFO("TextureManager: initialized — pool size={}, targetFmt=rgba8, semaphore={}",
             impl_->pool->capacity(),
             reinterpret_cast<void*>(impl_->holdSemaphore));
}

TextureManager::~TextureManager() {
    shutdown();
}

void TextureManager::shutdown() {
    if (impl_->holdSemaphore && impl_->gpu) {
        pl_vulkan_sem_destroy(impl_->gpu, &impl_->holdSemaphore);
        impl_->holdSemaphore = VK_NULL_HANDLE;
    }

    releaseUploadTextures();

    if (impl_->pool) {
        impl_->pool->drain();
        impl_->pool.reset();
    }

    LOG_INFO("TextureManager: shutdown complete");
}

void TextureManager::releaseUploadTextures() {
    for (auto& p : impl_->uploadPlanes) {
        if (p) {
            pl_tex_destroy(impl_->gpu, &p);
        }
    }
    impl_->uploadFrame    = {};
    impl_->cachedUploadFormat = AV_PIX_FMT_NONE;
    impl_->cachedUploadWidth  = 0;
    impl_->cachedUploadHeight = 0;
}

const pl_frame* TextureManager::uploadAvFrame(const AVFrame* avframe) {
    if (!avframe || !avframe->data[0]) {
        return nullptr;
    }

    AVPixelFormat avfmt = static_cast<AVPixelFormat>(avframe->format);
    FormatMeta meta;
    if (!getFormatMeta(avfmt, meta)) {
        return nullptr;
    }

    int w = avframe->width;
    int h = avframe->height;
    int numPlanes = meta.numPlanes;

    // 分辨率/格式变化时重建上传纹理
    if (avfmt != impl_->cachedUploadFormat
        || w != impl_->cachedUploadWidth
        || h != impl_->cachedUploadHeight) {
        releaseUploadTextures();

        for (int i = 0; i < numPlanes; ++i) {
            const PlaneMeta& pm = meta.planes[i];

            int pw = (w + (1 << pm.chromaShiftW) - 1) >> pm.chromaShiftW;
            int ph = (h + (1 << pm.chromaShiftH) - 1) >> pm.chromaShiftH;

            const char* fmtName = getPlaneFormatName(pm.bytesPerComp, pm.components);
            if (!fmtName) {
                LOG_ERROR("TextureManager: unsupported plane format — {} comps, {} bpc",
                          pm.components, pm.bytesPerComp);
                releaseUploadTextures();
                return nullptr;
            }

            pl_fmt fmt = pl_find_named_fmt(impl_->gpu, fmtName);
            if (!fmt) {
                LOG_ERROR("TextureManager: GPU does not support format '{}'", fmtName);
                releaseUploadTextures();
                return nullptr;
            }

            pl_tex_params tp = {};
            tp.w             = pw;
            tp.h             = ph;
            tp.format        = fmt;
            tp.sampleable    = true;
            tp.host_writable = true;

            impl_->uploadPlanes[i] = pl_tex_create(impl_->gpu, &tp);
            if (!impl_->uploadPlanes[i]) {
                LOG_ERROR("TextureManager: pl_tex_create() failed for plane {}", i);
                releaseUploadTextures();
                return nullptr;
            }
        }

        impl_->cachedUploadFormat = avfmt;
        impl_->cachedUploadWidth  = w;
        impl_->cachedUploadHeight = h;
    }

    // 逐平面上传
    for (int i = 0; i < numPlanes; ++i) {
        const PlaneMeta& pm = meta.planes[i];

        int pw = (w + (1 << pm.chromaShiftW) - 1) >> pm.chromaShiftW;
        int ph = (h + (1 << pm.chromaShiftH) - 1) >> pm.chromaShiftH;

        if (!impl_->uploadPlanes[i]) {
            return nullptr;
        }

        pl_tex_transfer_params ttp = {};
        ttp.tex       = impl_->uploadPlanes[i];
        ttp.rc        = { 0, 0, 0, pw, ph, 1 };
        ttp.row_pitch = static_cast<size_t>(avframe->linesize[i]);
        ttp.ptr       = avframe->data[i];

        if (!pl_tex_upload(impl_->gpu, &ttp)) {
            LOG_ERROR("TextureManager: pl_tex_upload() failed for plane {}", i);
            return nullptr;
        }
    }

    // 装配源 pl_frame
    impl_->uploadFrame = {};
    impl_->uploadFrame.num_planes = numPlanes;

    for (int i = 0; i < numPlanes; ++i) {
        const PlaneMeta& pm = meta.planes[i];

        impl_->uploadFrame.planes[i].texture    = impl_->uploadPlanes[i];
        impl_->uploadFrame.planes[i].components = pm.components;
        for (int c = 0; c < 4; ++c) {
            impl_->uploadFrame.planes[i].component_mapping[c] = pm.compMapping[c];
        }
    }

    impl_->uploadFrame.repr  = avToPlColorRepr(avframe);
    impl_->uploadFrame.color = avToPlColor(avframe);
    impl_->uploadFrame.crop  = { 0, 0, static_cast<float>(w), static_cast<float>(h) };

    return &impl_->uploadFrame;
}

pl_tex TextureManager::createTargetTex(int width, int height) {
    pl_tex_params tp = {};
    tp.w          = width;
    tp.h          = height;
    tp.format     = impl_->targetPlFmt;
    tp.sampleable = true;
    tp.renderable = true;

    pl_tex tex = pl_tex_create(impl_->gpu, &tp);
    if (!tex) {
        LOG_ERROR("TextureManager: failed to create target pl_tex ({}x{})", width, height);
    }
    return tex;
}

pl_tex TextureManager::wrapPoolImage(vk::Image image, int width, int height,
                                       vk::ImageLayout currentLayout,
                                       VkImageUsageFlags actualUsage) {
    pl_vulkan_wrap_params wp = {};
    wp.image  = static_cast<VkImage>(image);
    wp.width  = width;
    wp.height = height;
    wp.format = static_cast<VkFormat>(impl_->targetVkFormat);
    wp.usage  = actualUsage;

    pl_tex tex = pl_vulkan_wrap(impl_->gpu, &wp);
    if (!tex) {
        LOG_ERROR("TextureManager: pl_vulkan_wrap() failed for pooled VkImage ({}x{}, usage=0x{:x})",
                  width, height, actualUsage);
        return nullptr;
    }

    pl_vulkan_release_params rp = {};
    rp.tex    = tex;
    rp.layout = static_cast<VkImageLayout>(currentLayout);
    rp.qf     = impl_->queueFamily;

    pl_vulkan_release_ex(impl_->gpu, &rp);

    return tex;
}

void TextureManager::buildTargetFrame(pl_tex tex, int width, int height) {
    std::memset(&impl_->targetFrame, 0, sizeof(impl_->targetFrame));
    impl_->targetFrame.num_planes = 1;
    impl_->targetFrame.planes[0].texture    = tex;
    impl_->targetFrame.planes[0].components = 4;
    impl_->targetFrame.planes[0].component_mapping[0] = 0;
    impl_->targetFrame.planes[0].component_mapping[1] = 1;
    impl_->targetFrame.planes[0].component_mapping[2] = 2;
    impl_->targetFrame.planes[0].component_mapping[3] = 3;

    impl_->targetFrame.repr.sys        = PL_COLOR_SYSTEM_RGB;
    impl_->targetFrame.repr.levels     = PL_COLOR_LEVELS_PC;
    impl_->targetFrame.color.primaries = PL_COLOR_PRIM_BT_709;
    impl_->targetFrame.color.transfer  = PL_COLOR_TRC_SRGB;
    impl_->targetFrame.crop            = { 0, 0, static_cast<float>(width), static_cast<float>(height) };
}

TextureManager::TargetAcquisition TextureManager::acquireTarget(int width, int height) {
    TargetAcquisition result = {};

    vk::ImageLayout   poolLayout = vk::ImageLayout::eUndefined;
    VkImageUsageFlags poolUsage  = 0;
    vk::Image poolImage = impl_->pool->tryAcquire(width, height, impl_->targetVkFormat,
                                                   &poolLayout, &poolUsage);

    if (poolImage) {
        result.tex = wrapPoolImage(poolImage, width, height, poolLayout, poolUsage);
        if (!result.tex) {
            impl_->pool->release(poolImage);
        }
    }

    if (!result.tex) {
        result.tex = createTargetTex(width, height);
    }

    if (!result.tex) {
        return {};
    }

    buildTargetFrame(result.tex, width, height);
    result.frame = &impl_->targetFrame;

    return result;
}

vk::Image TextureManager::finalizeAndExport(pl_tex tex, int width, int height) {
    if (!tex) {
        LOG_ERROR("TextureManager: finalizeAndExport called with null tex");
        return nullptr;
    }

    pl_gpu_flush(impl_->gpu);

    pl_vulkan_hold_params holdParams = {};
    holdParams.tex       = tex;
    holdParams.layout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    holdParams.qf        = impl_->queueFamily;
    holdParams.semaphore = { impl_->holdSemaphore, 0 };
    if (!pl_vulkan_hold_ex(impl_->gpu, &holdParams)) {
        LOG_ERROR("TextureManager: pl_vulkan_hold_ex failed");
        return nullptr;
    }

    VkFormat           outFmt   = VK_FORMAT_UNDEFINED;
    VkImageUsageFlags  outUsage = 0;
    vk::Image vkImage = pl_vulkan_unwrap(impl_->gpu, tex, &outFmt, &outUsage);

    if (!vkImage) {
        LOG_ERROR("TextureManager: pl_vulkan_unwrap failed");
        return nullptr;
    }

    if (!impl_->pool->add(vkImage, width, height, impl_->targetVkFormat,
                          vk::ImageLayout::eShaderReadOnlyOptimal, outUsage)) {
        LOG_INFO("TextureManager: pool full, image will be destroyed on recycle");
    }

    return vkImage;
}

void TextureManager::discardTarget(pl_tex tex, int width, int height) {
    if (!tex) return;

    pl_vulkan_hold_params holdParams = {};
    holdParams.tex       = tex;
    holdParams.layout    = VK_IMAGE_LAYOUT_GENERAL;
    holdParams.qf        = impl_->queueFamily;
    holdParams.semaphore = { impl_->holdSemaphore, 0 };
    pl_vulkan_hold_ex(impl_->gpu, &holdParams);

    VkFormat          outFmt   = VK_FORMAT_UNDEFINED;
    VkImageUsageFlags outUsage = 0;
    vk::Image vkImage = pl_vulkan_unwrap(impl_->gpu, tex, &outFmt, &outUsage);

    if (!vkImage) return;

    impl_->pool->add(vkImage, width, height, impl_->targetVkFormat,
                     vk::ImageLayout::eUndefined, outUsage);
    impl_->pool->release(vkImage);
}

void TextureManager::recycleImage(vk::Image image) {
    if (!image) return;
    impl_->pool->release(image);
}

} // namespace renderer
} // namespace heisenberg
