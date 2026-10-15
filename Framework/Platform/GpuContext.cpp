//
// Created by NiceFold on 2026/6/30.
//

#include "GpuContext.hpp"
#include <Platform/Vulkan/VulkanQueueSync.hpp>
#include <Platform/Vulkan/VulkanContext.hpp>
#include <libplacebo/log.h>
#include <libplacebo/vulkan.h>
#include <volk.h>
#include <Utiles/Logger.hpp>
#include <stdexcept>
#include <cstring>

namespace heisenberg {
namespace renderer {

struct GpuContext::Impl {
    pl_log pl_logger = nullptr;
    pl_vulkan pl_vk = nullptr;
};

GpuContext::GpuContext(const VulkanResources& vkRes)
    : impl_(std::make_unique<Impl>()) {
    createLog();
    importVulkan(vkRes);
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
            case PL_LOG_FATAL:
                LOG_CRITICAL("[libplacebo] {}", msg);
                break;
            case PL_LOG_ERR:
                LOG_ERROR(   "[libplacebo] {}", msg);
                break;
            case PL_LOG_WARN:
                LOG_WARN(    "[libplacebo] {}", msg);
                break;
            case PL_LOG_INFO:
                LOG_INFO(    "[libplacebo] {}", msg);
                break;
            case PL_LOG_DEBUG:
                LOG_DEBUG(   "[libplacebo] {}", msg);
                break;
            case PL_LOG_TRACE:
                LOG_TRACE(   "[libplacebo] {}", msg);
                break;
            default:
                break;
        }
    };

    struct pl_log_params params = {};
    params.log_cb = logCallback;
#ifndef NDEBUG
    params.log_level = PL_LOG_DEBUG;
#else
    params.log_level = PL_LOG_INFO;
#endif

    impl_->pl_logger = pl_log_create(PL_API_VER, &params);
    if (!impl_->pl_logger) {
        throw std::runtime_error("pl_log_create() failed");
    }

    LOG_INFO("libplacebo: pl_log created");
}

void GpuContext::importVulkan(const VulkanResources& vkRes) {
    auto enabledFeatures = vkRes.enabledFeatures;
    auto extensions = vkRes.enabledExtensions;
    // Preserve callers using the shared device without guessing any capabilities.
    // An independent VkDevice must supply its own explicit enabled contract.
    if (!enabledFeatures) {
        auto& shared = VulkanContext::instance();
        if (vkRes.device != shared.device())
            throw std::invalid_argument("GpuContext import needs the actual enabled Vulkan feature contract");
        enabledFeatures = shared.enabledDeviceFeatures();
        extensions = shared.enabledDeviceExtensions();
    }
    if (!enabledFeatures)
        throw std::invalid_argument("GpuContext cannot import an uninitialized Vulkan device");

    struct pl_vulkan_import_params vp = {};
    vp.instance       = vkRes.instance;
    vp.get_proc_addr  = vkRes.getProcAddr;
    vp.phys_device    = vkRes.physDevice;
    vp.device         = vkRes.device;
    vp.extensions     = extensions.data();
    vp.num_extensions = static_cast<int>(extensions.size());
    vp.features       = enabledFeatures;
    vp.max_api_version = vkRes.apiVersion;
    vp.queue_graphics = { vkRes.graphicsQF, 1 };
    vp.queue_compute  = { 0 };
    vp.queue_transfer = { 0 };
    vp.lock_queue = lockVulkanQueue;
    vp.unlock_queue = unlockVulkanQueue;

    impl_->pl_vk = pl_vulkan_import(impl_->pl_logger, &vp);
    if (!impl_->pl_vk) {
        throw std::runtime_error("pl_vulkan_import() failed");
    }

    LOG_INFO("libplacebo: pl_vulkan imported from external device — maxTex2D={}",
             impl_->pl_vk->gpu->limits.max_tex_2d_dim);
}

pl_gpu GpuContext::plGpu() const {
    return impl_->pl_vk->gpu;
}

pl_vulkan GpuContext::plVulkan() const {
    return impl_->pl_vk;
}

} // namespace renderer
} // namespace heisenberg
