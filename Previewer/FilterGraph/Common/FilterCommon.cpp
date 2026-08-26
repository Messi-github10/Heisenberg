#include "FilterCommon.hpp"

extern "C" {
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
}

namespace heisenberg::filtergraph {
namespace {

ImageFormatDesc makePacked(FormatId id, const char* name, int components,
                           int bits, bool alpha, bool bgr,
                           bool floating, bool integer) {
    ImageFormatDesc desc;
    desc.id = id;
    desc.name = name;
    desc.flags = kFormatHasComponents | kFormatByteAligned | kFormatBytes;
    desc.flags |= floating ? kFormatTypeFloat : kFormatTypeUint;
    if (components <= 2) desc.flags |= kFormatGray;
    if (components >= 3) desc.flags |= kFormatColorRgb;
    if (alpha) desc.flags |= kFormatAlpha;
    desc.numPlanes = 1;
    desc.alignX = 1;
    desc.alignY = 1;
    desc.bpp[0] = static_cast<int16_t>(components * bits);

    for (int component = 0; component < components; ++component) {
        int semantic = component;
        if (bgr && component < 3) semantic = 2 - component;
        desc.components[semantic] = {
            0,
            static_cast<uint8_t>(component * bits),
            static_cast<uint8_t>(bits),
            0,
        };
    }
    if (!integer) desc.flags &= ~kFormatTypeUint;
    return desc;
}

struct PrivateFormatEntry {
    ImageFormatDesc desc;
    AVPixelFormat avFormat;
};

const auto& privateFormats() {
    static const PrivateFormatEntry formats[] = {
        {makePacked(static_cast<FormatId>(ImageType::r8), "r8", 1, 8,
                    false, false, false, true), AV_PIX_FMT_GRAY8},
        {makePacked(static_cast<FormatId>(ImageType::rgba8), "rgba8", 4, 8,
                    true, false, false, true), AV_PIX_FMT_RGBA},
        {makePacked(static_cast<FormatId>(ImageType::r16), "r16", 1, 16,
                    false, false, false, true), AV_PIX_FMT_GRAY16},
        {makePacked(static_cast<FormatId>(ImageType::bgra8), "bgra8", 4, 8,
                    true, true, false, true), AV_PIX_FMT_BGRA},
        {makePacked(static_cast<FormatId>(ImageType::rgba32f), "rgba32f", 4,
                    32, true, false, true, false), AV_PIX_FMT_RGBAF32},
        {makePacked(static_cast<FormatId>(ImageType::r32f), "r32f", 1, 32,
                    false, false, true, false), AV_PIX_FMT_GRAYF32},
        {makePacked(static_cast<FormatId>(ImageType::rgb8), "rgb8", 3, 8,
                    false, false, false, true), AV_PIX_FMT_RGB24},
        {makePacked(static_cast<FormatId>(ImageType::bgr8), "bgr8", 3, 8,
                    false, true, false, true), AV_PIX_FMT_BGR24},
        {makePacked(static_cast<FormatId>(ImageType::rgba32), "rgba32", 4,
                    32, true, false, false, true), AV_PIX_FMT_RGBA128},
        {makePacked(static_cast<FormatId>(ImageType::r32), "r32", 1, 32,
                    false, false, false, true), AV_PIX_FMT_GRAY32},
        {makePacked(static_cast<FormatId>(ImageType::rgba16f), "rgba16f", 4,
                    16, true, false, true, false), AV_PIX_FMT_RGBAF16},
    };
    return formats;
}

const ImageFormatDesc* findPrivate(FormatId id) {
    for (const PrivateFormatEntry& entry : privateFormats()) {
        if (entry.desc.id == id) return &entry.desc;
    }
    return nullptr;
}

AVPixelFormat toAvFormat(FormatId id) {
    if (!isFFmpegFormat(id)) return AV_PIX_FMT_NONE;
    const int32_t value = formatIdToFFmpeg(id);
    if (value < 0) return AV_PIX_FMT_NONE;
    const AVPixelFormat format = static_cast<AVPixelFormat>(value);
    return av_pix_fmt_desc_get(format) ? format : AV_PIX_FMT_NONE;
}

ImageFormatDesc fromAvDescriptor(FormatId id, AVPixelFormat format,
                                 const AVPixFmtDescriptor* avDesc) {
    ImageFormatDesc desc;
    const int numPlanes = av_pix_fmt_count_planes(format);
    if (avDesc->nb_components > 4 || numPlanes < 0 || numPlanes > 4)
        return desc;
    desc.id = id;
    desc.name = avDesc->name;
    desc.chromaXs = static_cast<int8_t>(avDesc->log2_chroma_w);
    desc.chromaYs = static_cast<int8_t>(avDesc->log2_chroma_h);
    desc.alignX = static_cast<int8_t>(1 << desc.chromaXs);
    desc.alignY = static_cast<int8_t>(1 << desc.chromaYs);
    desc.numPlanes = static_cast<int8_t>(numPlanes);

    if (avDesc->flags & AV_PIX_FMT_FLAG_ALPHA) desc.flags |= kFormatAlpha;
    if (avDesc->flags & AV_PIX_FMT_FLAG_RGB) {
        desc.flags |= kFormatColorRgb;
    } else if (!(avDesc->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
        desc.flags |= kFormatColorYuv;
    }
    if (avDesc->flags & AV_PIX_FMT_FLAG_FLOAT) {
        desc.flags |= kFormatTypeFloat;
    } else if (!(avDesc->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
        desc.flags |= kFormatTypeUint;
    }
    if (avDesc->flags & AV_PIX_FMT_FLAG_HWACCEL) {
        desc.flags |= kFormatTypeHardware;
    }

    for (int component = 0; component < avDesc->nb_components; ++component) {
        const AVComponentDescriptor& avComponent = avDesc->comp[component];
        if (avComponent.plane >= 4) continue;
        if (!desc.bpp[avComponent.plane]) {
            desc.bpp[avComponent.plane] = static_cast<int16_t>(
                avComponent.step * 8);
        }
        int semanticComponent = component;
        if ((avDesc->flags & AV_PIX_FMT_FLAG_ALPHA)
            && avDesc->nb_components < 4
            && component == avDesc->nb_components - 1) {
            semanticComponent = 3;
        }
        desc.components[semanticComponent] = {
            static_cast<uint8_t>(avComponent.plane),
            static_cast<uint8_t>(avComponent.offset * 8),
            static_cast<uint8_t>(avComponent.depth),
            0,
        };
    }

    desc.flags |= kFormatHasComponents;
    bool byteAligned = true;
    bool gray = avDesc->nb_components <= 2;
    for (int component = 0; component < 4; ++component) {
        byteAligned &= desc.components[component].offset % 8 == 0;
        byteAligned &= desc.components[component].size % 8 == 0;
        if (component == 1 && desc.components[component].size != 0)
            gray = false;
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
    if (name == "rgb30") name = "x2rgb10";

    for (const PrivateFormatEntry& entry : privateFormats()) {
        if (name == entry.desc.name) return entry.desc.id;
    }

    const std::string nameString(name);
    return imageFormatFromAvPixelFormat(
        static_cast<int32_t>(av_get_pix_fmt(nameString.c_str())));
}

const char* imageFormatName(FormatId id) {
    if (const ImageFormatDesc* desc = findPrivate(id)) return desc->name;
    if (const AVPixelFormat format = toAvFormat(id);
        format != AV_PIX_FMT_NONE) {
        if (const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(format))
            return desc->name;
    }
    return "unknown";
}

FormatId imageFormatFromAvPixelFormat(int32_t pixelFormat) {
    if (pixelFormat < 0) return toFormatId(ImageType::none);
    const AVPixelFormat format = static_cast<AVPixelFormat>(pixelFormat);
    if (!av_pix_fmt_desc_get(format)) return toFormatId(ImageType::none);

    for (const PrivateFormatEntry& entry : privateFormats()) {
        if (entry.avFormat == format) return entry.desc.id;
    }
    return formatIdFromFFmpeg(pixelFormat);
}

int32_t imageFormatToAvPixelFormat(FormatId id) {
    for (const PrivateFormatEntry& entry : privateFormats()) {
        if (entry.desc.id == id) return static_cast<int32_t>(entry.avFormat);
    }
    return static_cast<int32_t>(toAvFormat(id));
}

ImageFormatDesc imageFormatGetDesc(FormatId id) {
    if (const ImageFormatDesc* desc = findPrivate(id)) return *desc;
    const AVPixelFormat format = toAvFormat(id);
    const AVPixFmtDescriptor* avDesc = av_pix_fmt_desc_get(format);
    return avDesc ? fromAvDescriptor(id, format, avDesc) : ImageFormatDesc{};
}

} // namespace heisenberg::filtergraph
