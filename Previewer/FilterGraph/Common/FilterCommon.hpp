//
// Created by NiceFold on 2026/7/20.
//

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vulkan/vulkan.h>

namespace heisenberg {
namespace filtergraph {

enum class GraphicApiBackend : int32_t {
    other   = 0,
    vulkan  = 1,
};

using FormatId = int32_t;

enum class ImageType : int32_t {
    none = 0,
    start = 1000,

    // Planar YUV and gray formats.
    yuv444p,
    yuv420p,
    y8,
    y16,
    uyvy,
    nv12,
    p010,

    // Packed RGB/BGR formats.
    argb,
    bgra,
    abgr,
    rgba,
    bgr24,
    rgb24,
    x2rgb10,
    x2bgr10,
    rgbaf16,
    zero_rgb,
    bgr0,
    zero_bgr,
    rgb0,
    rgba64,
    rgb565,
    pal8,

    // Hardware-accelerated formats.
    vdpau,
    d3d11,
    dxva2,
    mmal,
    mediacodec,
    cuda,

    // MPV-specific descriptor table base and processing formats.
    cust_base,
    yap8,
    yap16,
    yapf,
    yuv444pf,
    yuv444apf,
    yuv420pf,
    yuv420apf,
    yuv422pf,
    yuv422apf,
    yuv440pf,
    yuv440apf,
    yuv410pf,
    yuv410apf,
    yuv411pf,
    yuv411apf,

    // Fringe formats used for RGB repacking.
    y1,
    gbrp1,
    gbrp2,
    gbrp3,
    gbrp4,
    gbrp5,
    gbrp6,

    // Additional hardware-accelerated formats.
    vdpau_output,
    vaapi,
    videotoolbox,
    vulkan,
    drmprime,

    // Project-specific formats not present in MPV's base list.
    heisenberg_rgba32f,
    heisenberg_r32f,
    heisenberg_rgba32,
    heisenberg_r32,

    // Generic pass-through of AV_PIX_FMT_* values.
    avpixfmt_start,
    avpixfmt_end = avpixfmt_start + 500,
    end,

    // Existing project names retained as compatibility aliases.
    other   = none,
    r8      = y8,
    r16     = y16,
    rgba8   = rgba,
    bgra8   = bgra,
    rgb8    = rgb24,
    bgr8    = bgr24,
    rgba16f = rgbaf16,
    rgba32f = heisenberg_rgba32f,
    r32f    = heisenberg_r32f,
    rgba32  = heisenberg_rgba32,
    r32     = heisenberg_r32,
};

constexpr FormatId toFormatId(ImageType type) noexcept {
    return static_cast<FormatId>(type);
}

constexpr bool isPrivateFormat(FormatId id) noexcept {
    return id >= toFormatId(ImageType::start) &&
           id <  toFormatId(ImageType::avpixfmt_start);
}

constexpr bool isFFmpegFormat(FormatId id) noexcept {
    return id >= toFormatId(ImageType::avpixfmt_start) &&
           id <  toFormatId(ImageType::avpixfmt_end);
}

// AV_PIX_FMT_NONE is -1. Invalid values are represented by ImageType::none.
constexpr FormatId formatIdFromFFmpeg(int32_t pixelFormat) noexcept {
    constexpr FormatId begin = toFormatId(ImageType::avpixfmt_start);
    constexpr FormatId end = toFormatId(ImageType::avpixfmt_end);
    return pixelFormat >= 0 && pixelFormat < (end - begin)
        ? begin + pixelFormat
        : toFormatId(ImageType::none);
}

// Returns -1 for non-FFmpeg handles, matching AV_PIX_FMT_NONE without making
// this common header depend on FFmpeg headers.
constexpr int32_t formatIdToFFmpeg(FormatId id) noexcept {
    constexpr FormatId begin = toFormatId(ImageType::avpixfmt_start);
    return isFFmpegFormat(id) ? id - begin : -1;
}

enum ImageFormatFlags : uint32_t {
    kFormatHasComponents = 1u << 0,
    kFormatBytes         = 1u << 1,
    kFormatByteAligned   = 1u << 2,
    kFormatAlpha         = 1u << 3,
    kFormatColorYuv      = 1u << 4,
    kFormatColorRgb      = 1u << 5,
    kFormatTypeUint      = 1u << 6,
    kFormatTypeFloat     = 1u << 7,
    kFormatTypeHardware  = 1u << 8,
    kFormatGray          = 1u << 9,
};

struct ImageFormatComponentDesc {
    uint8_t plane  = 0;
    uint8_t offset = 0;
    uint8_t size   = 0;
    int8_t  pad    = 0;
};

struct ImageFormatDesc {
    FormatId id = toFormatId(ImageType::none);
    const char* name = nullptr;
    uint32_t flags = 0;
    int8_t numPlanes = 0;
    int8_t chromaXs = 0;
    int8_t chromaYs = 0;
    int8_t alignX = 1;
    int8_t alignY = 1;
    int16_t bpp[4] = {};
    int8_t xs[4] = {};
    int8_t ys[4] = {};
    ImageFormatComponentDesc components[4] = {};
};

FormatId imageFormatFromName(std::string_view name);
const char* imageFormatName(FormatId id);
FormatId imageFormatFromAvPixelFormat(int32_t pixelFormat);
int32_t imageFormatToAvPixelFormat(FormatId id);
ImageFormatDesc imageFormatGetDesc(FormatId id);

enum class ColorPrimaries : int32_t {
    unknown      = 0,
    bt601_525    = 1,
    bt601_625    = 2,
    bt709        = 3,
    bt470m       = 4,
    ebu_3213     = 5,
    bt2020       = 6,
    apple        = 7,
    adobe        = 8,
    pro_photo    = 9,
    cie_1931     = 10,
    dci_p3       = 11,
    display_p3   = 12,
    v_gamut      = 13,
    s_gamut      = 14,
    film_c       = 15,
    aces_ap0     = 16,
    aces_ap1     = 17,
    count        = 18,
};

enum class ColorTransfer : int32_t {
    unknown   = 0,
    bt1886    = 1,
    srgb      = 2,
    linear    = 3,
    gamma18   = 4,
    gamma20   = 5,
    gamma22   = 6,
    gamma24   = 7,
    gamma26   = 8,
    gamma28   = 9,
    pro_photo = 10,
    st428     = 11,
    pq        = 12,
    hlg       = 13,
    v_log     = 14,
    s_log1    = 15,
    s_log2    = 16,
    scrgb     = 17,
    count     = 18,
};

enum class AlphaMode : int32_t {
    unknown       = 0,
    independent   = 1,
    premultiplied = 2,
    none          = 3,
    count         = 4,
    straight      = independent,
};

// DAG 滤镜图内部的像素语义契约
struct GraphImageContract {
    VkFormat       format        = VK_FORMAT_UNDEFINED;         // Vulkan Image 的物理格式
    ImageType      imageType     = ImageType::other;            // 图像格式
    ColorPrimaries primaries     = ColorPrimaries::unknown;     // 色彩空间
    ColorTransfer  transfer      = ColorTransfer::unknown;      // 传递函数
    AlphaMode      alpha         = AlphaMode::unknown;          // Alpha 通道

    constexpr bool operator==(const GraphImageContract&) const = default;
};

// 运行时语义契约
inline constexpr GraphImageContract kWorkingImageContract{
    VK_FORMAT_R16G16B16A16_SFLOAT,
    ImageType::rgba16f,
    ColorPrimaries::bt2020,
    ColorTransfer::linear,
    AlphaMode::straight,
};

// 图像格式
struct ImageFormat {
    int32_t  width     = 0;
    int32_t  height    = 0;
    ImageType imageType = ImageType::other;

    inline bool operator==(const ImageFormat& right) const {
        return this->width     == right.width
            && this->height    == right.height
            && this->imageType == right.imageType;
    }

    inline bool operator!=(const ImageFormat& right) const {
        return !(*this == right);
    }
};

struct VideoFormat {
    int32_t   width     = 0;
    int32_t   height    = 0;
    int32_t   fps       = 0;
    FormatId  format    = toFormatId(ImageType::none);
};

struct VulkanSyncPoint {
    VkSemaphore semaphore = VK_NULL_HANDLE;
    uint64_t    value     = 0;

    bool valid() const { return semaphore != VK_NULL_HANDLE; }
};

struct VulkanImageRef {
    VkImage           image            = VK_NULL_HANDLE;
    VkImageView       view             = VK_NULL_HANDLE;
    VkFormat          format           = VK_FORMAT_UNDEFINED;
    VkExtent2D        extent           = {};
    VkImageUsageFlags usage            = 0;
    VkImageLayout     layout           = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t          queueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    VulkanSyncPoint   ready            = {};
    uint64_t          generation       = 0;
    GraphImageContract contract         = {};

    bool valid() const {
        return image != VK_NULL_HANDLE
            && format != VK_FORMAT_UNDEFINED
            && extent.width > 0
            && extent.height > 0;
    }
};

struct VulkanGraphContext {
    VkInstance       instance          = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice    = VK_NULL_HANDLE;
    VkDevice         device            = VK_NULL_HANDLE;
    VkQueue          queue             = VK_NULL_HANDLE;
    uint32_t         queueFamilyIndex  = VK_QUEUE_FAMILY_IGNORED;
};

struct FrameContext {
    int64_t pts          = 0;
    int32_t timeBaseNum  = 0;
    int32_t timeBaseDen  = 1;
    int64_t frameIndex   = -1;
    double  timeSeconds  = 0.0;
};

} // namespace filtergraph
} // namespace heisenberg
