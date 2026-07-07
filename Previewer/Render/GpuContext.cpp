//
// Created by NiceFold on 2026/6/30.
//

#include "GpuContext.hpp"
#include "VulkanContext.hpp"
#include <libplacebo/log.h>
#include <libplacebo/vulkan.h>
#include <volk.h>
#include <Utiles/Logger.hpp>
#include <stdexcept>

namespace heisenberg {
namespace render {

struct GpuContext::Impl {
    pl_log    pl_logger = nullptr;
    pl_vulkan pl_vk     = nullptr;
};

GpuContext::GpuContext()
    : impl_(std::make_unique<Impl>()) {
    createLog();
    importVulkan();
}

GpuContext::~GpuContext() {
    if (impl_->pl_vk) {
        pl_vulkan_destroy(&impl_->pl_vk);
    }
    if (impl_->pl_logger) {
        pl_log_destroy(&impl_->pl_logger);
    }
}

void GpuContext::createLog() {
    static const auto logCallback = [](void* /*priv*/, enum pl_log_level level, const char* msg) {
        switch (level) {
        case PL_LOG_FATAL: LOG_CRITICAL("[libplacebo] {}", msg); break;
        case PL_LOG_ERR:   LOG_ERROR(   "[libplacebo] {}", msg); break;
        case PL_LOG_WARN:  LOG_WARN(    "[libplacebo] {}", msg); break;
        case PL_LOG_INFO:  LOG_INFO(    "[libplacebo] {}", msg); break;
        case PL_LOG_DEBUG: LOG_DEBUG(   "[libplacebo] {}", msg); break;
        case PL_LOG_TRACE: LOG_TRACE(   "[libplacebo] {}", msg); break;
        default: break;
        }
    };

    struct pl_log_params params = {};
    params.log_cb    = logCallback;
    params.log_level = PL_LOG_INFO;

    impl_->pl_logger = pl_log_create(PL_API_VER, &params);
    if (!impl_->pl_logger) {
        throw std::runtime_error("pl_log_create() failed");
    }

    LOG_INFO("libplacebo: pl_log created");
}

void GpuContext::importVulkan() {
    auto& vk = VulkanContext::instance();

    VkPhysicalDeviceFeatures2 features2 =
        { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    VkPhysicalDeviceVulkan11Features vk11 =
        { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
    VkPhysicalDeviceVulkan12Features vk12 =
        { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };

    features2.pNext = &vk11;
    vk11.pNext      = &vk12;

    vkGetPhysicalDeviceFeatures2(vk.vkPhysicalDevice(), &features2);

    struct pl_vulkan_import_params vp = {};
    vp.instance       = vk.vkInstance();
    vp.get_proc_addr  = vk.getInstanceProcAddr();
    vp.phys_device    = vk.vkPhysicalDevice();
    vp.device         = vk.vkDevice();
    vp.extensions     = vk.deviceExtensions().data();
    vp.num_extensions = static_cast<int>(vk.deviceExtensions().size());
    vp.features       = &features2;
    vp.queue_graphics = { vk.graphicsQueueFamily(), 1 };
    vp.queue_compute  = { 0 };
    vp.queue_transfer = { 0 };

    impl_->pl_vk = pl_vulkan_import(impl_->pl_logger, &vp);
    if (!impl_->pl_vk) {
        throw std::runtime_error("pl_vulkan_import() failed");
    }

    LOG_INFO("libplacebo: pl_vulkan imported — maxTex2D={}",
             impl_->pl_vk->gpu->limits.max_tex_2d_dim);
}

pl_gpu GpuContext::gpu() const {
    return impl_->pl_vk->gpu;
}

pl_vulkan GpuContext::vulkan() const {
    return impl_->pl_vk;
}

} // namespace render
} // namespace heisenberg
