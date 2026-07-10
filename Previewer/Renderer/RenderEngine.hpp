//
// Created by NiceFold on 2026/6/30.
//

#pragma once

#include <memory>
#include <Common/NonCopy.hpp>

extern "C" {
#include <libplacebo/renderer.h>
#include <libplacebo/gpu.h>
}

namespace heisenberg {
namespace renderer {

class RenderEngine : public NonCopy {
public:
    explicit RenderEngine(pl_gpu gpu);
    ~RenderEngine();

    RenderEngine(RenderEngine&&)                 = delete;
    RenderEngine& operator=(RenderEngine&&)      = delete;

    /// 将源图像渲染到目标帧缓冲。
    /// @param image  源帧
    /// @param target 目标帧
    /// @param params 渲染参数
    /// @return true 表示渲染成功
    bool render(const pl_frame* image, const pl_frame* target,
                const pl_render_params* params = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace renderer
} // namespace heisenberg
