//
// Created by NiceFold on 2026/7/20.
//

#pragma once

#include <cstdint>
#include <string>
#include <vulkan/vulkan.h>

namespace heisenberg {
namespace filtergraph {

enum class GpuType : int32_t {
    other   = 0,
    vulkan  = 1,
};

enum class ImageType : int32_t {
    other    = 0,
    r8       = 1,
    rgba8    = 2,
    r16      = 3,
    bgra8    = 4,
    rgba32f  = 5,
    r32f     = 6,
    rgb8     = 7,
    bgr8     = 8,
    rgba32   = 9,
    r32      = 10,
    rgba16f  = 11,
};

enum class ColorPrimaries : int32_t {
    unknown = 0,
    bt2020  = 1,
};

enum class ColorTransfer : int32_t {
    unknown = 0,
    linear  = 1,
};

enum class AlphaMode : int32_t {
    unknown  = 0,
    straight = 1,
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

enum class VideoType : int32_t {
    other   = 0,
    nv12    = 1,
    yuv2I   = 2,
    yvyuI   = 3,
    uyvyI   = 4,
    rgba8   = 5,
    bgra8   = 6,
    yuy2P   = 7,
    yuv420P = 8,
    rgb8    = 9,
};

struct VideoFormat {
    int32_t   width     = 0;
    int32_t   height    = 0;
    int32_t   fps       = 0;
    VideoType videoType = VideoType::other;
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
