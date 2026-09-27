#include "FilterCommon.hpp"

extern "C" {
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
}

namespace heisenberg::filtergraph {
namespace {

struct FormatAlias {
    std::string_view alias;
    std::string_view canonicalName;
};

constexpr FormatAlias kFormatAliases[] = {
    {"rgb30", "x2rgb10"},
    {"r8", "y8"}, {"r16", "y16"},
    {"rgba8", "rgba"}, {"bgra8", "bgra"},
    {"rgb8", "rgb24"}, {"bgr8", "bgr24"},
    {"rgba16f", "rgbaf16"},
};

ImageFormatDesc makePacked(FormatId id, const char* name, int components,
                           int bits, bool alpha, bool bgr,
                           bool floating, VkFormat vkFormat) {
    ImageFormatDesc desc;
    desc.id = id;
    desc.name = name;
    desc.vkFormat = vkFormat;
    desc.flags = kFormatHasComponents | kFormatByteAligned | kFormatBytes;
    desc.flags |= floating ? kFormatTypeFloat : kFormatTypeUint;
    if (components <= 2) desc.flags |= kFormatGray;
    if (components >= 3) desc.flags |= kFormatColorRgb;
    if (alpha) desc.flags |= kFormatAlpha;
    desc.numPlanes = 1;
    desc.bpp[0] = static_cast<int16_t>(components * bits);
    for (int component = 0; component < components; ++component) {
        int semantic = bgr && component < 3 ? 2 - component : component;
        desc.components[semantic] = {
            0, static_cast<uint8_t>(component * bits),
            static_cast<uint8_t>(bits), 0};
    }
    return desc;
}

ImageFormatDesc makePlanar(FormatId id, const char* name, int components,
                           int bits, int chromaXs, int chromaYs, bool alpha,
                           bool floating) {
    ImageFormatDesc desc;
    desc.id = id;
    desc.name = name;
    desc.flags = kFormatHasComponents | kFormatByteAligned | kFormatBytes
        | kFormatColorYuv;
    desc.flags |= floating ? kFormatTypeFloat : kFormatTypeUint;
    if (components <= 2) desc.flags |= kFormatGray;
    if (alpha) desc.flags |= kFormatAlpha;
    desc.numPlanes = static_cast<int8_t>(components);
    desc.chromaXs = static_cast<int8_t>(chromaXs);
    desc.chromaYs = static_cast<int8_t>(chromaYs);
    desc.alignX = static_cast<int8_t>(1 << chromaXs);
    desc.alignY = static_cast<int8_t>(1 << chromaYs);
    for (int component = 0; component < components; ++component) {
        const int semantic = alpha && component == components - 1 ? 3 : component;
        desc.bpp[component] = static_cast<int16_t>(bits);
        desc.xs[component] = component > 0 && component < 3
            ? static_cast<int8_t>(chromaXs) : 0;
        desc.ys[component] = component > 0 && component < 3
            ? static_cast<int8_t>(chromaYs) : 0;
        desc.components[semantic] = {
            static_cast<uint8_t>(component), 0, static_cast<uint8_t>(bits), 0};
    }
    return desc;
}

ImageFormatDesc makeHardware(FormatId id, const char* name) {
    ImageFormatDesc desc;
    desc.id = id;
    desc.name = name;
    desc.flags = kFormatTypeHardware;
    return desc;
}

struct PrivateFormatEntry {
    ImageFormatDesc desc;
    AVPixelFormat avFormat = AV_PIX_FMT_NONE;
};

PrivateFormatEntry mapped(FormatId id, const char* name,
                          AVPixelFormat avFormat,
                          VkFormat vkFormat = VK_FORMAT_UNDEFINED) {
    ImageFormatDesc desc;
    desc.id = id;
    desc.name = name;
    desc.vkFormat = vkFormat;
    return {desc, avFormat};
}

PrivateFormatEntry mapped(ImageType type, const char* name,
                          AVPixelFormat avFormat,
                          VkFormat vkFormat = VK_FORMAT_UNDEFINED) {
    return mapped(toFormatId(type), name, avFormat, vkFormat);
}

PrivateFormatEntry described(ImageFormatDesc desc,
                             AVPixelFormat avFormat = AV_PIX_FMT_NONE) {
    return {desc, avFormat};
}

const auto& privateFormats() {
    static const PrivateFormatEntry formats[] = {
        mapped(ImageType::yuv444p, "yuv444p", AV_PIX_FMT_YUV444P),
        mapped(ImageType::yuv420p, "yuv420p", AV_PIX_FMT_YUV420P),
        mapped(ImageType::y8, "y8", AV_PIX_FMT_GRAY8, VK_FORMAT_R8_UNORM),
        mapped(ImageType::y16, "y16", AV_PIX_FMT_GRAY16, VK_FORMAT_R16_UNORM),
        mapped(ImageType::uyvy, "uyvy", AV_PIX_FMT_UYVY422),
        mapped(ImageType::nv12, "nv12", AV_PIX_FMT_NV12),
        mapped(ImageType::p010, "p010", AV_PIX_FMT_P010),
        mapped(ImageType::argb, "argb", AV_PIX_FMT_ARGB),
        mapped(ImageType::bgra, "bgra", AV_PIX_FMT_BGRA,
               VK_FORMAT_B8G8R8A8_UNORM),
        mapped(ImageType::abgr, "abgr", AV_PIX_FMT_ABGR),
        mapped(ImageType::rgba, "rgba", AV_PIX_FMT_RGBA,
               VK_FORMAT_R8G8B8A8_UNORM),
        mapped(ImageType::bgr24, "bgr24", AV_PIX_FMT_BGR24),
        mapped(ImageType::rgb24, "rgb24", AV_PIX_FMT_RGB24),
        mapped(ImageType::x2rgb10, "x2rgb10", AV_PIX_FMT_X2RGB10),
        mapped(ImageType::x2bgr10, "x2bgr10", AV_PIX_FMT_X2BGR10),
        mapped(ImageType::rgbaf16, "rgbaf16", AV_PIX_FMT_RGBAF16,
               VK_FORMAT_R16G16B16A16_SFLOAT),
        mapped(ImageType::zero_rgb, "0rgb", AV_PIX_FMT_0RGB),
        mapped(ImageType::bgr0, "bgr0", AV_PIX_FMT_BGR0),
        mapped(ImageType::zero_bgr, "0bgr", AV_PIX_FMT_0BGR),
        mapped(ImageType::rgb0, "rgb0", AV_PIX_FMT_RGB0),
        mapped(ImageType::rgba64, "rgba64", AV_PIX_FMT_RGBA64),
        mapped(ImageType::rgb565, "rgb565", AV_PIX_FMT_RGB565),
        mapped(ImageType::pal8, "pal8", AV_PIX_FMT_PAL8),
        mapped(ImageType::vdpau, "vdpau", AV_PIX_FMT_VDPAU),
        mapped(ImageType::d3d11, "d3d11", AV_PIX_FMT_D3D11),
        mapped(ImageType::dxva2, "dxva2", AV_PIX_FMT_DXVA2_VLD),
        mapped(ImageType::mmal, "mmal", AV_PIX_FMT_MMAL),
        mapped(ImageType::mediacodec, "mediacodec", AV_PIX_FMT_MEDIACODEC),
        mapped(ImageType::cuda, "cuda", AV_PIX_FMT_CUDA),
        described(makePlanar(toFormatId(ImageType::yap8), "yap8",
                             2, 8, 0, 0, true, false)),
        described(makePlanar(toFormatId(ImageType::yap16), "yap16",
                             2, 16, 0, 0, true, false)),
        described(makePlanar(toFormatId(ImageType::yapf), "grayaf32",
                             2, 32, 0, 0, true, true)),
        described(makePlanar(toFormatId(ImageType::yuv444pf), "yuv444pf",
                             3, 32, 0, 0, false, true)),
        described(makePlanar(toFormatId(ImageType::yuv444apf), "yuva444pf",
                             4, 32, 0, 0, true, true)),
        described(makePlanar(toFormatId(ImageType::yuv420pf), "yuv420pf",
                             3, 32, 1, 1, false, true)),
        described(makePlanar(toFormatId(ImageType::yuv420apf), "yuva420pf",
                             4, 32, 1, 1, true, true)),
        described(makePlanar(toFormatId(ImageType::yuv422pf), "yuv422pf",
                             3, 32, 1, 0, false, true)),
        described(makePlanar(toFormatId(ImageType::yuv422apf), "yuva422pf",
                             4, 32, 1, 0, true, true)),
        described(makePlanar(toFormatId(ImageType::yuv440pf), "yuv440pf",
                             3, 32, 0, 1, false, true)),
        described(makePlanar(toFormatId(ImageType::yuv440apf), "yuva440pf",
                             4, 32, 0, 1, true, true)),
        described(makePlanar(toFormatId(ImageType::yuv410pf), "yuv410pf",
                             3, 32, 2, 2, false, true)),
        described(makePlanar(toFormatId(ImageType::yuv410apf), "yuva410pf",
                             4, 32, 2, 2, true, true)),
        described(makePlanar(toFormatId(ImageType::yuv411pf), "yuv411pf",
                             3, 32, 2, 0, false, true)),
        described(makePlanar(toFormatId(ImageType::yuv411apf), "yuva411pf",
                             4, 32, 2, 0, true, true)),
        described(makePacked(toFormatId(ImageType::y1), "y1", 1, 1,
                             false, false, false, VK_FORMAT_UNDEFINED)),
        described(makePlanar(toFormatId(ImageType::gbrp1), "gbrp1",
                             3, 1, 0, 0, false, false)),
        described(makePlanar(toFormatId(ImageType::gbrp2), "gbrp2",
                             3, 2, 0, 0, false, false)),
        described(makePlanar(toFormatId(ImageType::gbrp3), "gbrp3",
                             3, 3, 0, 0, false, false)),
        described(makePlanar(toFormatId(ImageType::gbrp4), "gbrp4",
                             3, 4, 0, 0, false, false)),
        described(makePlanar(toFormatId(ImageType::gbrp5), "gbrp5",
                             3, 5, 0, 0, false, false)),
        described(makePlanar(toFormatId(ImageType::gbrp6), "gbrp6",
                             3, 6, 0, 0, false, false)),
        described(makeHardware(toFormatId(ImageType::vdpau_output),
                               "vdpau_output")),
        mapped(ImageType::vaapi, "vaapi", AV_PIX_FMT_VAAPI),
        mapped(ImageType::videotoolbox, "videotoolbox",
               AV_PIX_FMT_VIDEOTOOLBOX),
        mapped(ImageType::vulkan, "vulkan", AV_PIX_FMT_VULKAN),
        mapped(ImageType::drmprime, "drmprime", AV_PIX_FMT_DRM_PRIME),
        described(makePacked(toFormatId(ImageType::rgba32f), "rgba32f",
                             4, 32, true, false, true,
                             VK_FORMAT_R32G32B32A32_SFLOAT), AV_PIX_FMT_RGBAF32),
        described(makePacked(toFormatId(ImageType::r32f), "r32f", 1, 32,
                             false, false, true, VK_FORMAT_R32_SFLOAT),
                  AV_PIX_FMT_GRAYF32),
        described(makePacked(toFormatId(ImageType::rgba32), "rgba32", 4, 32,
                             true, false, false, VK_FORMAT_UNDEFINED),
                  AV_PIX_FMT_RGBA128),
        described(makePacked(toFormatId(ImageType::r32), "r32", 1, 32,
                             false, false, false, VK_FORMAT_R32_UINT),
                  AV_PIX_FMT_GRAY32),
    };
    return formats;
}

const PrivateFormatEntry* findPrivate(FormatId id) {
    for (const PrivateFormatEntry& entry : privateFormats())
        if (entry.desc.id == id) return &entry;
    return nullptr;
}

const PrivateFormatEntry* findPrivate(std::string_view name) {
    for (const PrivateFormatEntry& entry : privateFormats())
        if (name == entry.desc.name) return &entry;
    return nullptr;
}

AVPixelFormat toAvFormat(FormatId id) {
    if (const PrivateFormatEntry* entry = findPrivate(id))
        return entry->avFormat;
    return isFFmpegFormat(id)
        ? static_cast<AVPixelFormat>(formatIdToFFmpeg(id))
        : AV_PIX_FMT_NONE;
}

VkFormat vkFormatFromAv(AVPixelFormat format) {
    switch (format) {
        case AV_PIX_FMT_GRAY8: return VK_FORMAT_R8_UNORM;
        case AV_PIX_FMT_GRAY16: return VK_FORMAT_R16_UNORM;
        case AV_PIX_FMT_RGBA: return VK_FORMAT_R8G8B8A8_UNORM;
        case AV_PIX_FMT_BGRA: return VK_FORMAT_B8G8R8A8_UNORM;
        case AV_PIX_FMT_RGBAF16: return VK_FORMAT_R16G16B16A16_SFLOAT;
        case AV_PIX_FMT_GRAYF32: return VK_FORMAT_R32_SFLOAT;
        case AV_PIX_FMT_RGBAF32: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case AV_PIX_FMT_GRAY32: return VK_FORMAT_R32_UINT;
        default: return VK_FORMAT_UNDEFINED;
    }
}

ImageFormatDesc fromAvDescriptor(FormatId id, AVPixelFormat format,
                                 const AVPixFmtDescriptor* avDesc) {
    ImageFormatDesc desc;
    const int numPlanes = av_pix_fmt_count_planes(format);
    if (avDesc->nb_components > 4 || numPlanes < 0 || numPlanes > 4)
        return desc;
    desc.id = id;
    desc.name = avDesc->name;
    desc.vkFormat = vkFormatFromAv(format);
    desc.chromaXs = static_cast<int8_t>(avDesc->log2_chroma_w);
    desc.chromaYs = static_cast<int8_t>(avDesc->log2_chroma_h);
    desc.alignX = static_cast<int8_t>(1 << desc.chromaXs);
    desc.alignY = static_cast<int8_t>(1 << desc.chromaYs);
    desc.numPlanes = static_cast<int8_t>(numPlanes);
    if (avDesc->flags & AV_PIX_FMT_FLAG_ALPHA) desc.flags |= kFormatAlpha;
    if (avDesc->flags & AV_PIX_FMT_FLAG_RGB) desc.flags |= kFormatColorRgb;
    else if (!(avDesc->flags & AV_PIX_FMT_FLAG_HWACCEL))
        desc.flags |= kFormatColorYuv;
    if (avDesc->flags & AV_PIX_FMT_FLAG_FLOAT) desc.flags |= kFormatTypeFloat;
    else if (!(avDesc->flags & AV_PIX_FMT_FLAG_HWACCEL))
        desc.flags |= kFormatTypeUint;
    if (avDesc->flags & AV_PIX_FMT_FLAG_HWACCEL)
        desc.flags |= kFormatTypeHardware;

    for (int component = 0; component < avDesc->nb_components; ++component) {
        const AVComponentDescriptor& avComponent = avDesc->comp[component];
        if (avComponent.plane >= 4) continue;
        if (!desc.bpp[avComponent.plane])
            desc.bpp[avComponent.plane] =
                static_cast<int16_t>(avComponent.step * 8);
        int semantic = component;
        if ((avDesc->flags & AV_PIX_FMT_FLAG_ALPHA)
            && avDesc->nb_components < 4
            && component == avDesc->nb_components - 1) semantic = 3;
        desc.components[semantic] = {
            static_cast<uint8_t>(avComponent.plane),
            static_cast<uint8_t>(avComponent.offset * 8),
            static_cast<uint8_t>(avComponent.depth), 0};
    }

    desc.flags |= kFormatHasComponents;
    bool byteAligned = true;
    bool gray = avDesc->nb_components <= 2;
    for (int component = 0; component < 4; ++component) {
        byteAligned &= desc.components[component].offset % 8 == 0;
        byteAligned &= desc.components[component].size % 8 == 0;
        if (component == 1 && desc.components[component].size != 0) gray = false;
    }
    if (byteAligned) desc.flags |= kFormatByteAligned | kFormatBytes;
    if (gray) desc.flags |= kFormatGray;
    for (int plane = 1; plane < desc.numPlanes && plane < 4; ++plane) {
        desc.xs[plane] = desc.chromaXs;
        desc.ys[plane] = desc.chromaYs;
    }
    return desc;
}

} // namespace

FormatId imageFormatFromName(std::string_view name) {
    if (name == "none") return toFormatId(ImageType::none);
    for (const FormatAlias& alias : kFormatAliases) {
        if (name == alias.alias) {
            name = alias.canonicalName;
            break;
        }
    }
    if (const PrivateFormatEntry* entry = findPrivate(name))
        return entry->desc.id;
    const std::string nameString(name);
    return imageFormatFromAvPixelFormat(
        static_cast<int32_t>(av_get_pix_fmt(nameString.c_str())));
}

const char* imageFormatName(FormatId id) {
    if (const PrivateFormatEntry* entry = findPrivate(id))
        return entry->desc.name;
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(toAvFormat(id));
    return desc ? desc->name : "unknown";
}

FormatId imageFormatFromAvPixelFormat(int32_t pixelFormat) {
    if (pixelFormat < 0) return toFormatId(ImageType::none);
    const AVPixelFormat format = static_cast<AVPixelFormat>(pixelFormat);
    for (const PrivateFormatEntry& entry : privateFormats())
        if (entry.avFormat == format) return entry.desc.id;
    return formatIdFromFFmpeg(pixelFormat);
}

int32_t imageFormatToAvPixelFormat(FormatId id) {
    return static_cast<int32_t>(toAvFormat(id));
}

FormatId imageFormatFromVkFormat(VkFormat vkFormat) {
    if (vkFormat == VK_FORMAT_UNDEFINED) return toFormatId(ImageType::none);
    for (const PrivateFormatEntry& entry : privateFormats())
        if (entry.desc.vkFormat == vkFormat) return entry.desc.id;
    return toFormatId(ImageType::none);
}

VkFormat imageFormatToVkFormat(FormatId id) {
    return imageFormatGetDesc(id).vkFormat;
}

ImageFormatDesc imageFormatGetDesc(FormatId id) {
    if (const PrivateFormatEntry* entry = findPrivate(id)) {
        if (entry->desc.flags || entry->avFormat == AV_PIX_FMT_NONE)
            return entry->desc;
        const AVPixFmtDescriptor* avDesc = av_pix_fmt_desc_get(entry->avFormat);
        if (!avDesc) return entry->desc;
        ImageFormatDesc desc = fromAvDescriptor(id, entry->avFormat, avDesc);
        desc.name = entry->desc.name;
        if (entry->desc.vkFormat != VK_FORMAT_UNDEFINED)
            desc.vkFormat = entry->desc.vkFormat;
        return desc;
    }
    if (!isFFmpegFormat(id)) return {};
    const AVPixelFormat format = static_cast<AVPixelFormat>(formatIdToFFmpeg(id));
    const AVPixFmtDescriptor* avDesc = av_pix_fmt_desc_get(format);
    return avDesc ? fromAvDescriptor(id, format, avDesc) : ImageFormatDesc{};
}

} // namespace heisenberg::filtergraph
