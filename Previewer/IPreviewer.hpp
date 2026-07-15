//
// Created by NiceFold on 2026/7/9.
//

#pragma once

#include <memory>
#include <functional>
#include <Common/NonCopy.hpp>

extern "C" {
#include <libplacebo/gpu.h>
}

struct AVFrame;

namespace heisenberg {
namespace renderer {

class SwapChain;
class TextureManager;
class RenderEngine;

class IPreviewer : public NonCopy {
public:
    using ResizeCallback  = std::function<void(int width, int height)>;
    using PresentCallback = std::function<void()>;

    IPreviewer();
    ~IPreviewer();

    IPreviewer(IPreviewer&&)                 = delete;
    IPreviewer& operator=(IPreviewer&&)      = delete;

    /// @param gpu        来自 GpuContext 的 pl_gpu
    /// @param swapChain  已初始化好的 SwapChain（由外部创建并注入）
    /// @param width, height 初始尺寸
    bool initialize(pl_gpu gpu, std::unique_ptr<SwapChain> swapChain,
                    int width, int height);

    /// 渲染一帧 → swapchain 呈现
    bool presentFrame(const AVFrame* frame);

    void resize(int width, int height);

    void setOnResize(ResizeCallback cb);
    void setOnPresent(PresentCallback cb);

    void shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace renderer
} // namespace heisenberg
