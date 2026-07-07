//
// Created by NiceFold on 2026/6/30.
//

#pragma once

#include <memory>

extern "C" {
#include <libplacebo/swapchain.h>
#include <libplacebo/vulkan.h>
#include <libplacebo/renderer.h>
}

#include <vulkan/vulkan.h>

namespace heisenberg {
namespace render {

class SwapChain {
public:
    SwapChain(pl_vulkan vk, VkSurfaceKHR surface, int width, int height);
    ~SwapChain();

    SwapChain(const SwapChain&)            = delete;
    SwapChain& operator=(const SwapChain&) = delete;
    SwapChain(SwapChain&&)                 = delete;
    SwapChain& operator=(SwapChain&&)      = delete;

    bool startFrame();

    const pl_frame* getTargetFrame() const;

    bool submitFrame();

    void swapBuffers();

    bool resize(int* width, int* height);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace render
} // namespace heisenberg
