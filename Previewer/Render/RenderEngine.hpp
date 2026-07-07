//
// Created by NiceFold on 2026/6/30.
//

#pragma once

#include <memory>

extern "C" {
#include <libplacebo/renderer.h>
#include <libplacebo/gpu.h>
}

namespace heisenberg {
namespace render {

/// RenderEngine — pl_renderer 的 RAII 封装
///
/// 将 TextureTransfer 产生的 pl_frame 渲染到 SwapChain 提供的目标帧缓冲。
class RenderEngine {
public:
    explicit RenderEngine(pl_gpu gpu);
    ~RenderEngine();

    RenderEngine(const RenderEngine&)            = delete;
    RenderEngine& operator=(const RenderEngine&) = delete;
    RenderEngine(RenderEngine&&)                 = delete;
    RenderEngine& operator=(RenderEngine&&)      = delete;

    /// 将源图像渲染到目标帧缓冲。
    /// @param image  源帧（来自 TextureTransfer::uploadAVFrame），可为 nullptr
    /// @param target 目标帧（来自 SwapChain::getTargetFrame）
    /// @param params 渲染参数，nullptr = 使用默认高质量参数
    /// @return true 表示渲染成功
    bool render(const pl_frame* image, const pl_frame* target,
                const pl_render_params* params = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace render
} // namespace heisenberg
