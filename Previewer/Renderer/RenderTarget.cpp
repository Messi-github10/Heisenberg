//
// Created by NiceFold on 2026/7/10.
//

#include "RenderTarget.hpp"
#include <volk.h>
#include <libplacebo/vulkan.h>
#include <Utiles/Logger.hpp>
#include <cstring>

namespace heisenberg {
namespace renderer {

RenderTarget::RenderTarget(pl_gpu gpu, uint32_t queueFamily)
    : gpu_(gpu)
    , queueFamily_(queueFamily) {
    pl_vulkan_sem_params semParams = {};
    semParams.type = VK_SEMAPHORE_TYPE_BINARY;
    holdSemaphore_ = pl_vulkan_sem_create(gpu_, &semParams);
    if (!holdSemaphore_) {
        throw std::runtime_error("RenderTarget: failed to create hold semaphore");
    }
    LOG_INFO("RenderTarget: created (semaphore={})",
             reinterpret_cast<void*>(holdSemaphore_));
}

RenderTarget::~RenderTarget() {
    destroyCurrentTex();
    if (holdSemaphore_ && gpu_) {
        pl_vulkan_sem_destroy(gpu_, &holdSemaphore_);
    }
}

void RenderTarget::destroyCurrentTex() {
    if (tex_ && gpu_) {
        pl_tex_destroy(gpu_, &tex_);
    }
    targetFrame_ = {};
}

void RenderTarget::resize(int width, int height) {
    if (width == width_ && height == height_ && tex_) {
        return;
    }

    destroyCurrentTex();

    pl_fmt fmt = pl_find_named_fmt(gpu_, "rgba8");
    if (!fmt) {
        LOG_ERROR("RenderTarget: GPU does not support rgba8 format");
        return;
    }

    pl_tex_params tp = {};
    tp.w          = width;
    tp.h          = height;
    tp.format     = fmt;
    tp.sampleable = true;
    tp.renderable = true;

    tex_ = pl_tex_create(gpu_, &tp);
    if (!tex_) {
        LOG_ERROR("RenderTarget: failed to create target pl_tex ({}x{})", width, height);
        return;
    }

    std::memset(&targetFrame_, 0, sizeof(targetFrame_));
    targetFrame_.num_planes = 1;
    targetFrame_.planes[0].texture   = tex_;
    targetFrame_.planes[0].components = 4;
    targetFrame_.planes[0].component_mapping[0] = 0;
    targetFrame_.planes[0].component_mapping[1] = 1;
    targetFrame_.planes[0].component_mapping[2] = 2;
    targetFrame_.planes[0].component_mapping[3] = 3;

    targetFrame_.repr.sys    = PL_COLOR_SYSTEM_RGB;
    targetFrame_.repr.levels = PL_COLOR_LEVELS_PC;
    targetFrame_.color.primaries = PL_COLOR_PRIM_BT_709;
    targetFrame_.color.transfer  = PL_COLOR_TRC_SRGB;

    width_  = width;
    height_ = height;
}

vk::Image RenderTarget::finalize() {
    if (!tex_) {
        LOG_ERROR("RenderTarget: finalize called without target texture");
        return {};
    }

    // 提交所有 GPU 命令到 Vulkan 队列
    pl_gpu_flush(gpu_);

    // 转换 image layout，让 Qt 能采样
    pl_vulkan_hold_params holdParams = {};
    holdParams.tex       = tex_;
    holdParams.layout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    holdParams.qf        = queueFamily_;
    holdParams.semaphore = { holdSemaphore_, 0 };
    if (!pl_vulkan_hold_ex(gpu_, &holdParams)) {
        LOG_ERROR("RenderTarget: pl_vulkan_hold_ex failed");
        return {};
    }

    // 取出底层 VkImage 并且销毁 pl_tex ，所有权转移给调用者
    vk::Image vkImage = pl_vulkan_unwrap(gpu_, tex_, nullptr, nullptr);
    tex_ = nullptr;
    targetFrame_ = {};

    if (!vkImage) {
        LOG_ERROR("RenderTarget: pl_vulkan_unwrap failed");
        return {};
    }

    // vk::Image 由调用者通过 IPreviewer::destroyVkImage 延迟销毁
    return vkImage;
}

} // namespace renderer
} // namespace heisenberg
