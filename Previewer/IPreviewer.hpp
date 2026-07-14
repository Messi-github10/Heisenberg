//
// Created by NiceFold on 2026/7/9.
//

#pragma once

#include <memory>
#include <functional>
#include <vulkan/vulkan.hpp>
#include <Common/NonCopy.hpp>

extern "C" {
#include <libplacebo/gpu.h>
}

struct AVFrame;

namespace heisenberg {
namespace renderer {

/// 渲染一帧的输出 — 从渲染层传递给 UI 层的 Vulkan 纹理句柄
struct FrameOutput {
    vk::Image      image;
    vk::ImageLayout layout = vk::ImageLayout::eShaderReadOnlyOptimal;
    int            width  = 0;
    int            height = 0;
};

class IPreviewer : public NonCopy {
public:
    using ResizeCallback  = std::function<void(int width, int height)>;
    using PresentCallback = std::function<void()>;

    IPreviewer();
    ~IPreviewer();

    IPreviewer(IPreviewer&&)                 = delete;
    IPreviewer& operator=(IPreviewer&&)      = delete;

    /// 初始化渲染管线
    /// @param gpu     来自 GpuContext 的 pl_gpu
    /// @param device  共享的 vk::Device，用于延迟销毁 vk::Image
    /// @param queueFamily 图形队列族索引
    /// @param width   初始宽度
    /// @param height  初始高度
    /// @return true 表示初始化成功
    bool initialize(pl_gpu gpu, vk::Device device,
                    uint32_t queueFamily, int width, int height);

    /// 渲染一帧，返回可用于 Qt 场景图采样的 FrameOutput
    /// @param frame FFmpeg 解码的 AVFrame
    /// @return FrameOutput，image 为空表示失败
    FrameOutput presentFrame(const AVFrame* frame);

    void resize(int width, int height);
    void recycleVkImage(vk::Image image);

    void setOnResize(ResizeCallback cb);
    void setOnPresent(PresentCallback cb);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace renderer
} // namespace heisenberg
