//
// Created by NiceFold on 2026/7/14.
//

#include "SoftwareContext.hpp"
#include <Video/Renderer/ColorSpaceUtils.hpp>

#include <libplacebo/colorspace.h>
#include <Utiles/Logger.hpp>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
}

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
    { AV_PIX_FMT_RGBAF16,     {0, 0, 0,  0}, 16, 0, 0 },
};

static const char* getPlaneFormatName(int bytesPerComp, int components) {
    if (components == 4 && bytesPerComp == 2) return "rgba16hf";
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

    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(avfmt);
    if (desc) {
        out = deriveFromFFmpeg(desc, avfmt);
        return true;
    }

    LOG_ERROR("SoftwareContext: unsupported pixel format {}", static_cast<int>(avfmt));
    return false;
}



} // anonymous namespace

struct SoftwareContext::Impl {
    pl_gpu gpu = nullptr;
    bool shutdownDone = false;

    pl_tex       uploadPlanes[4] = {};
    pl_frame     uploadFrame     = {};
    AVPixelFormat cachedUploadFormat = AV_PIX_FMT_NONE;
    int          cachedUploadWidth  = 0;
    int          cachedUploadHeight = 0;
};

SoftwareContext::SoftwareContext(pl_gpu gpu)
    : impl_(std::make_unique<Impl>()) {
    impl_->gpu = gpu;
    LOG_INFO("SoftwareContext: initialized");
}

SoftwareContext::~SoftwareContext() {
    shutdown();
}

void SoftwareContext::shutdown() {
    if (!impl_ || impl_->shutdownDone) return;

    impl_->shutdownDone = true;
    releaseUploadTextures();
    impl_->gpu = nullptr;
    LOG_INFO("SoftwareContext: shutdown complete");
}

void SoftwareContext::releaseUploadTextures() {
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

const pl_frame* SoftwareContext::uploadAvFrame(const AVFrame* avframe) {
    if (!impl_ || impl_->shutdownDone || !impl_->gpu
        || !avframe || !avframe->data[0]) {
        return nullptr;
    }

    if (pl_gpu_is_failed(impl_->gpu)) {
        LOG_ERROR("SoftwareContext: GPU is in failed state, cannot upload");
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
                LOG_ERROR("SoftwareContext: unsupported plane format — {} comps, {} bpc",
                          pm.components, pm.bytesPerComp);
                releaseUploadTextures();
                return nullptr;
            }

            pl_fmt fmt = pl_find_named_fmt(impl_->gpu, fmtName);
            if (!fmt && std::strcmp(fmtName, "rgba16hf") == 0) {
                fmt = pl_find_named_fmt(impl_->gpu, "rgba16f");
            }
            if (!fmt) {
                LOG_ERROR("SoftwareContext: GPU does not support format '{}'", fmtName);
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
                LOG_ERROR("SoftwareContext: pl_tex_create() failed for plane {}", i);
                releaseUploadTextures();
                return nullptr;
            }
            if (avfmt == AV_PIX_FMT_RGBAF16 &&
                impl_->uploadPlanes[i]->params.format &&
                impl_->uploadPlanes[i]->params.format->texel_size != 8) {
                LOG_ERROR("SoftwareContext: GPU rgba16f texel_size={} does not match RGBAF16",
                          impl_->uploadPlanes[i]->params.format->texel_size);
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
            LOG_ERROR("SoftwareContext: pl_tex_upload() failed for plane {}", i);
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

    impl_->uploadFrame.repr  = colorReprFromAvFrame(avframe);
    impl_->uploadFrame.color = colorSpaceFromAvFrame(avframe);
    impl_->uploadFrame.crop  = { 0, 0, static_cast<float>(w), static_cast<float>(h) };

    return &impl_->uploadFrame;
}

} // namespace renderer
} // namespace heisenberg
