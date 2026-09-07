#pragma once

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <libplacebo/colorspace.h>
}

#include <algorithm>

namespace heisenberg::renderer {

inline pl_color_system colorSystemFromAvFrame(const AVFrame* frame) {
    if (!frame) return PL_COLOR_SYSTEM_UNKNOWN;
    switch (frame->colorspace) {
    case AVCOL_SPC_RGB:
        return PL_COLOR_SYSTEM_RGB;
    case AVCOL_SPC_BT709:
        return PL_COLOR_SYSTEM_BT_709;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
    case AVCOL_SPC_SMPTE240M:
        return PL_COLOR_SYSTEM_BT_601;
    case AVCOL_SPC_BT2020_NCL:
        return PL_COLOR_SYSTEM_BT_2020_NC;
    case AVCOL_SPC_BT2020_CL:
        return PL_COLOR_SYSTEM_BT_2020_C;
    default:
        break;
    }

    if (frame->color_primaries == AVCOL_PRI_BT2020)
        return PL_COLOR_SYSTEM_BT_2020_NC;
    if (frame->color_primaries == AVCOL_PRI_BT709)
        return PL_COLOR_SYSTEM_BT_709;
    return pl_color_system_guess_ycbcr(frame->width, frame->height);
}

inline pl_color_space colorSpaceFromAvFrame(const AVFrame* frame) {
    pl_color_space color{};
    if (!frame) return color;

    switch (frame->color_primaries) {
    case AVCOL_PRI_BT709:
        color.primaries = PL_COLOR_PRIM_BT_709;
        break;
    case AVCOL_PRI_BT470M:
        color.primaries = PL_COLOR_PRIM_BT_470M;
        break;
    case AVCOL_PRI_BT470BG:
        color.primaries = PL_COLOR_PRIM_BT_601_625;
        break;
    case AVCOL_PRI_SMPTE170M:
    case AVCOL_PRI_SMPTE240M:
        color.primaries = PL_COLOR_PRIM_BT_601_525;
        break;
    case AVCOL_PRI_BT2020:
        color.primaries = PL_COLOR_PRIM_BT_2020;
        break;
    case AVCOL_PRI_SMPTE431:
        color.primaries = PL_COLOR_PRIM_DCI_P3;
        break;
    case AVCOL_PRI_SMPTE432:
        color.primaries = PL_COLOR_PRIM_DISPLAY_P3;
        break;
    default:
        break;
    }

    switch (frame->color_trc) {
    case AVCOL_TRC_BT709:
        color.transfer = PL_COLOR_TRC_BT_1886;
        break;
    case AVCOL_TRC_IEC61966_2_1:
        color.transfer = PL_COLOR_TRC_SRGB;
        break;
    case AVCOL_TRC_LINEAR:
        color.transfer = PL_COLOR_TRC_LINEAR;
        break;
    case AVCOL_TRC_GAMMA22:
        color.transfer = PL_COLOR_TRC_GAMMA22;
        break;
    case AVCOL_TRC_GAMMA28:
        color.transfer = PL_COLOR_TRC_GAMMA28;
        break;
    case AVCOL_TRC_SMPTE2084:
        color.transfer = PL_COLOR_TRC_PQ;
        break;
    case AVCOL_TRC_ARIB_STD_B67:
        color.transfer = PL_COLOR_TRC_HLG;
        break;
    default:
        break;
    }

    // Unspecified primaries/transfer are common in SDR files. Infer them
    // from the matrix before falling back to libplacebo's defaults, so the
    // software and D3D11 paths describe the same source frame.
    const pl_color_system system = colorSystemFromAvFrame(frame);
    if (color.primaries == PL_COLOR_PRIM_UNKNOWN) {
        if (system == PL_COLOR_SYSTEM_BT_601)
            color.primaries = PL_COLOR_PRIM_BT_601_625;
        else if (system == PL_COLOR_SYSTEM_BT_709)
            color.primaries = PL_COLOR_PRIM_BT_709;
        else if (system == PL_COLOR_SYSTEM_BT_2020_NC
                 || system == PL_COLOR_SYSTEM_BT_2020_C)
            color.primaries = PL_COLOR_PRIM_BT_2020;
    }
    if (color.transfer == PL_COLOR_TRC_UNKNOWN
        && system != PL_COLOR_SYSTEM_RGB)
        color.transfer = PL_COLOR_TRC_BT_1886;

    pl_color_space_infer(&color);
    return color;
}

inline pl_color_repr colorReprFromAvFrame(const AVFrame* frame) {
    pl_color_repr repr{};
    if (!frame) return repr;

    const auto* desc = av_pix_fmt_desc_get(
        static_cast<AVPixelFormat>(frame->format));
    if (desc) {
        repr.bits.sample_depth = desc->comp[0].depth;
        repr.bits.color_depth = desc->comp[0].depth;
        for (int c = 1; c < desc->nb_components; ++c)
            repr.bits.color_depth =
                std::max(repr.bits.color_depth, desc->comp[c].depth);
    }
    repr.levels = frame->color_range == AVCOL_RANGE_JPEG
        ? PL_COLOR_LEVELS_PC : PL_COLOR_LEVELS_TV;
    repr.sys = colorSystemFromAvFrame(frame);
    return repr;
}

inline pl_color_space workingColorSpace() {
    pl_color_space color{};
    color.primaries = PL_COLOR_PRIM_BT_2020;
    color.transfer = PL_COLOR_TRC_LINEAR;
    pl_color_space_infer(&color);
    return color;
}

} // namespace heisenberg::renderer
