//
// Created by NiceFold on 2026/7/15.
//

#pragma once

#include <memory>
#include <Common/NonCopy.hpp>
#include <vulkan/vulkan.hpp>

extern "C" {
#include <libplacebo/swapchain.h>
#include <libplacebo/vulkan.h>
}

namespace heisenberg {
namespace renderer {

class SwapChain : public NonCopy {
public:
    SwapChain();
    ~SwapChain();

    SwapChain(SwapChain&&)            = delete;
    SwapChain& operator=(SwapChain&&) = delete;

    bool initialize(pl_vulkan plVk, vk::Instance vkInst,
                    void* hwnd, int width, int height);
    void shutdown();

    pl_tex startFrame(int width, int height);

    bool submitFrame();
    void swapBuffers();
    bool resize(int* width, int* height);

    bool isValid() const { return sw_ != nullptr; }

private:
    pl_swapchain  sw_      = nullptr;
    VkSurfaceKHR  surface_ = VK_NULL_HANDLE;
    vk::Instance  instance_;
};

} // namespace renderer
} // namespace heisenberg
