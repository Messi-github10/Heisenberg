//
// Created by NiceFold on 2026/7/10.
//

#pragma once

#include <vulkan/vulkan.hpp>
#include <Common/NonCopy.hpp>

extern "C" {
#include <libplacebo/gpu.h>
#include <libplacebo/renderer.h>
}

namespace heisenberg {
namespace renderer {

class RenderTarget : public NonCopy {
public:
    RenderTarget(pl_gpu gpu, uint32_t queueFamily);
    ~RenderTarget();

    RenderTarget(RenderTarget&&)                 = delete;
    RenderTarget& operator=(RenderTarget&&)      = delete;

    void resize(int width, int height);
    vk::Image finalize();

    const pl_frame* targetFrame() const { return &targetFrame_; }
    int width()  const { return width_; }
    int height() const { return height_; }

private:
    void destroyCurrentTex();

    pl_gpu   gpu_   = nullptr;
    uint32_t queueFamily_ = 0;

    pl_tex   tex_ = nullptr;
    pl_frame targetFrame_ = {};

    VkSemaphore holdSemaphore_  = VK_NULL_HANDLE;

    int width_  = 0;
    int height_ = 0;
};

} // namespace renderer
} // namespace heisenberg
