//
// Created by NiceFold on 2026/6/30.
//

#include "SwapChain.hpp"
#include <libplacebo/swapchain.h>
#include <libplacebo/vulkan.h>
#include <libplacebo/renderer.h>
#include <volk.h>
#include <Utiles/Logger.hpp>
#include <stdexcept>
#include <cassert>

namespace heisenberg {
namespace render {


struct SwapChain::Impl {
    pl_vulkan    vk      = nullptr;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    pl_swapchain sw      = nullptr;

    pl_swapchain_frame sw_frame     = {};
    pl_frame           target_frame = {};
    bool               frame_started = false;
};

SwapChain::SwapChain(pl_vulkan vk, VkSurfaceKHR surface, int width, int height)
    : impl_(std::make_unique<Impl>()) {
    impl_->vk      = vk;
    impl_->surface = surface;

    struct pl_vulkan_swapchain_params vsp = {};
    vsp.surface       = surface;
    vsp.present_mode  = VK_PRESENT_MODE_FIFO_KHR;
    vsp.swapchain_depth = 3;

    impl_->sw = pl_vulkan_create_swapchain(vk, &vsp);
    if (!impl_->sw) {
        throw std::runtime_error("pl_vulkan_create_swapchain() failed");
    }

    // 不在构造函数中 resize — 延迟到第一次 startFrame
    // 让窗口有足够时间完成初始化（处理消息泵等）
    LOG_INFO("SwapChain: wrapper created (swapchain deferred)");
}

SwapChain::~SwapChain() {
    if (impl_->sw) {
        pl_swapchain_destroy(&impl_->sw);
    }
    if (impl_->surface != VK_NULL_HANDLE && impl_->vk) {
        vkDestroySurfaceKHR(impl_->vk->instance, impl_->surface, nullptr);
    }
}

bool SwapChain::startFrame() {
    assert(!impl_->frame_started && "mismatched startFrame/submitFrame");

    bool ok = pl_swapchain_start_frame(impl_->sw, &impl_->sw_frame);
    if (!ok) {
        static int failCount = 0;
        if (failCount++ < 3) {
            LOG_WARN("SwapChain: pl_swapchain_start_frame failed");
        }
        return false;
    }

    pl_frame_from_swapchain(&impl_->target_frame, &impl_->sw_frame);
    impl_->frame_started = true;
    return true;
}

const pl_frame* SwapChain::getTargetFrame() const {
    return &impl_->target_frame;
}

bool SwapChain::submitFrame() {
    assert(impl_->frame_started && "submitFrame without startFrame");

    impl_->frame_started = false;
    return pl_swapchain_submit_frame(impl_->sw);
}

void SwapChain::swapBuffers() {
    pl_swapchain_swap_buffers(impl_->sw);
}

bool SwapChain::resize(int* width, int* height) {
    int reqW = *width, reqH = *height;
    bool ok = pl_swapchain_resize(impl_->sw, width, height);
    LOG_INFO("SwapChain: resize request={}x{} actual={}x{} result={}",
             reqW, reqH, *width, *height, ok ? "OK" : "FAIL");
    return ok;
}

} // namespace render
} // namespace heisenberg
