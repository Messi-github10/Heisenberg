//
// Created by NiceFold on 2026/7/20.
//

#pragma once

#include <cstdint>
#include <string>
#include <vulkan/vulkan.h>

namespace heisenberg {
namespace filtergraph {

// ============================================================
// GPU 类型
// ============================================================

enum class GpuType : int32_t {
    other   = 0,
    vulkan  = 1,
};

// ============================================================
// 图像类型（对应 shader 里的存储格式）
// ============================================================

enum class ImageType : int32_t {
    other    = 0,
    r8       = 1,   // 单通道 8-bit
    rgba8    = 2,   // 四通道 8-bit
    r16      = 3,   // 单通道 16-bit
    bgra8    = 4,   // BGRA 四通道 8-bit
    rgba32f  = 5,   // 四通道 32-bit 浮点
    r32f     = 6,   // 单通道 32-bit 浮点
    rgb8     = 7,   // 三通道 8-bit
    bgr8     = 8,   // 三通道 8-bit (BGR)
    rgba32   = 9,   // 四通道 32-bit 整数
    r32      = 10,  // 单通道 32-bit 整数
    rgba16f  = 11,  // 四通道 16-bit 浮点
};

// ============================================================
// 图像格式描述
// ============================================================

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

// ============================================================
// 图内节点/引脚索引
// ============================================================

struct NodeIndex {
    int32_t nodeIndex = -1;  // 图级节点索引
    int32_t siteIndex = -1;  // 节点上的第几个输入/输出 pin
};

// ============================================================
// 视频类型（输入层使用）
// ============================================================

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

// ============================================================
// 视频格式描述
// ============================================================

struct VideoFormat {
    int32_t   index     = -1;
    int32_t   width     = 0;
    int32_t   height    = 0;
    int32_t   fps       = 0;
    VideoType videoType = VideoType::other;
};

// ============================================================
// Vulkan 共享资源
// ============================================================

struct VulkanSyncPoint {
    VkSemaphore semaphore = VK_NULL_HANDLE;
    uint64_t    value     = 0;

    bool valid() const { return semaphore != VK_NULL_HANDLE; }
};

/// A borrowed Vulkan image. The receiver must not destroy image or view.
/// `ready` is signaled when `layout` and `queueFamilyIndex` become valid.
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

    bool valid() const {
        return image != VK_NULL_HANDLE
            && format != VK_FORMAT_UNDEFINED
            && extent.width > 0
            && extent.height > 0;
    }
};

/// Vulkan objects are borrowed from the renderer and outlive the graph.
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

// ============================================================
// CPU 视频帧
// ============================================================

struct VideoFrame {
    int32_t   width;
    int32_t   height;
    int64_t   timeStamp;
    VideoType videoType = VideoType::other;
    uint8_t*  data[4]       = {};
    int32_t   dataAlign[4]  = {};
};

} // namespace filtergraph
} // namespace heisenberg
