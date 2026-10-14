#pragma once

#include "Rational.hpp"

extern "C" {
#include <libavutil/pixfmt.h>
}

namespace heisenberg {

enum class WorkingFormat { Rgba8, Rgba16f, Rgba32f };
enum class ColorPrimaries { Bt709, Bt2020, DisplayP3 };
enum class ColorTransfer { Bt1886, Srgb, Pq, Hlg, Linear };
enum class ColorRange { Full, Limited };

const char* toString(WorkingFormat format);
const char* toString(ColorPrimaries primaries);
const char* toString(ColorTransfer transfer);
const char* toString(ColorRange range);

// Source/backend metadata. No Qt or project/settings dependency is required.
struct VideoSpec {
    int width = 0;
    int height = 0;
    Rational frameRate;
    Rational sampleAspect{1, 1};
    Rational timeBase{0, 1};
    AVPixelFormat format = AV_PIX_FMT_NONE;
    AVColorPrimaries colorPrimaries = AVCOL_PRI_UNSPECIFIED;
    AVColorTransferCharacteristic colorTransfer = AVCOL_TRC_UNSPECIFIED;
    AVColorSpace colorSpace = AVCOL_SPC_UNSPECIFIED;
    AVColorRange colorRange = AVCOL_RANGE_UNSPECIFIED;
    AVChromaLocation chromaLocation = AVCHROMA_LOC_UNSPECIFIED;

    bool valid() const { return width > 0 && height > 0; }
};

} // namespace heisenberg
