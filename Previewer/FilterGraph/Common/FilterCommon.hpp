//
// Created by NiceFold on 2026/7/20.
//

#pragma once

#include <cstdint>
#include <string>

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
// 输出纹理句柄（给 libplacebo 的 VkImage 导出）
// ============================================================

struct VkOutGpuTex {
    void*  commandBuffer = nullptr;
    void*  image         = nullptr;   // VkImage
    int32_t width        = 0;
    int32_t height       = 0;
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
