//
// Created by NiceFold on 2026/7/15.
//

#include "SwapChain.hpp"
#include <Utiles/Logger.hpp>
#include <volk.h>

namespace heisenberg {
namespace renderer {

SwapChain::SwapChain()  = default;
SwapChain::~SwapChain() { shutdown(); }

bool SwapChain::initialize(pl_vulkan plVk, vk::Instance vkInst,
                           void* hwnd, int width, int height) {
    instance_ = vkInst;

    VkWin32SurfaceCreateInfoKHR surfaceInfo = {};
    surfaceInfo.sType     = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    surfaceInfo.hinstance = GetModuleHandle(nullptr);
    surfaceInfo.hwnd      = reinterpret_cast<HWND>(hwnd);

    VkInstance rawInst = static_cast<VkInstance>(vkInst);
    PFN_vkCreateWin32SurfaceKHR fpCreateSurface =
        reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(
            vkGetInstanceProcAddr(rawInst, "vkCreateWin32SurfaceKHR"));
    if (!fpCreateSurface) {
        LOG_ERROR("SwapChain: vkCreateWin32SurfaceKHR not available");
        return false;
    }

    VkResult res = fpCreateSurface(rawInst, &surfaceInfo, nullptr, &surface_);
    if (res != VK_SUCCESS) {
        LOG_ERROR("SwapChain: vkCreateWin32SurfaceKHR failed (VkResult={})",
                  static_cast<int>(res));
        return false;
    }

    pl_vulkan_swapchain_params swParams = {};
    swParams.surface         = surface_;
    swParams.present_mode    = VK_PRESENT_MODE_FIFO_KHR;
    swParams.swapchain_depth = 3;

    sw_ = pl_vulkan_create_swapchain(plVk, &swParams);
    if (!sw_) {
        LOG_ERROR("SwapChain: pl_vulkan_create_swapchain failed");
        vkDestroySurfaceKHR(rawInst, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
        return false;
    }

    int w = width, h = height;
    resize(&w, &h);

    LOG_INFO("SwapChain: created — {}x{}", w, h);
    return true;
}

void SwapChain::shutdown() {
    if (sw_) {
        pl_swapchain_destroy(&sw_);
    }
    if (surface_ && instance_) {
        vkDestroySurfaceKHR(static_cast<VkInstance>(instance_), surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }
}

pl_tex SwapChain::startFrame(int width, int height) {
    if (!sw_) return nullptr;

    int w = width, h = height;
    if (!pl_swapchain_resize(sw_, &w, &h)) {
        return nullptr;
    }

    struct pl_swapchain_frame swFrame;
    if (!pl_swapchain_start_frame(sw_, &swFrame)) {
        return nullptr;
    }

    return swFrame.fbo;
}

bool SwapChain::submitFrame() {
    if (!sw_) return false;
    return pl_swapchain_submit_frame(sw_);
}

void SwapChain::swapBuffers() {
    if (!sw_) return;
    pl_swapchain_swap_buffers(sw_);
}

bool SwapChain::resize(int* width, int* height) {
    if (!sw_) return false;
    if (!width || !height || *width <= 0 || *height <= 0) return false;
    return pl_swapchain_resize(sw_, width, height);
}

} // namespace renderer
} // namespace heisenberg
