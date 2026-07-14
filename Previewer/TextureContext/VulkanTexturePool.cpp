//
// Created by NiceFold on 2026/7/14.
//

#include "VulkanTexturePool.hpp"
#include <Utiles/Logger.hpp>

namespace heisenberg {
namespace renderer {

VulkanTexturePool::VulkanTexturePool(vk::Device device, size_t maxSize)
    : maxSize_(maxSize)
    , device_(device) {
    texturePool_.reserve(maxSize_);
}

VulkanTexturePool::~VulkanTexturePool() {
    if (!texturePool_.empty()) {
        LOG_WARN("VulkanTexturePool: destroyed without drain — leaking {} VkImage(s)",
                 texturePool_.size());
    }
}

void VulkanTexturePool::destroyCachedTexture(CachedTexture& cachedTexture) {
    if (cachedTexture.image && device_) {
        device_.destroyImage(cachedTexture.image);
        cachedTexture.image = nullptr;
    }
}

vk::Image VulkanTexturePool::tryAcquire(int width, int height, vk::Format format,
                                         vk::ImageLayout* outLayout,
                                         VkImageUsageFlags* outUsage) {
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& texture : texturePool_) {
        if (!texture.inUse && texture.image
            && texture.width  == width
            && texture.height == height
            && texture.format == format) {
            texture.inUse = true;
            if (outLayout) *outLayout = texture.currentLayout;
            if (outUsage)  *outUsage  = texture.usageFlags;
            return texture.image;
        }
    }
    return nullptr;
}

void VulkanTexturePool::release(vk::Image image) {
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& texture : texturePool_) {
        if (texture.image == image) {
            texture.inUse = false;
            return;
        }
    }

    LOG_WARN("VulkanTexturePool: release() called for untracked VkImage — destroying it");
    if (image && device_) {
        device_.destroyImage(image);
    }
}

bool VulkanTexturePool::add(vk::Image image, int width, int height, vk::Format format,
                             vk::ImageLayout layout, VkImageUsageFlags usage) {
    std::lock_guard<std::mutex> lock(mutex_);

    // 去重：已在池中则更新 layout 并标记 in-use
    for (auto& texture : texturePool_) {
        if (texture.image == image) {
            texture.currentLayout = layout;
            texture.usageFlags    = usage;
            texture.inUse         = true;
            return true;
        }
    }

    // 池满时踢出并销毁一个空闲条目
    if (texturePool_.size() >= maxSize_) {
        for (auto& texture : texturePool_) {
            if (!texture.inUse) {
                LOG_INFO("VulkanTexturePool: evicting old VkImage ({}x{}) for new ({}x{})",
                         texture.width, texture.height, width, height);
                destroyCachedTexture(texture);
                texture.image  = nullptr;
                texture.inUse  = false;
                texture.width  = 0;
                texture.height = 0;
                texture.format = vk::Format::eUndefined;
                break;
            }
        }
    }

    if (texturePool_.size() >= maxSize_) {
        LOG_WARN("VulkanTexturePool: pool full ({}), all in-use — discarding new image",
                 maxSize_);
        return false;
    }

    CachedTexture cachedTexture;
    cachedTexture.image         = image;
    cachedTexture.width         = width;
    cachedTexture.height        = height;
    cachedTexture.format        = format;
    cachedTexture.currentLayout = layout;
    cachedTexture.usageFlags    = usage;
    cachedTexture.inUse         = true;
    texturePool_.push_back(cachedTexture);

    return true;
}

void VulkanTexturePool::drain() {
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& texture : texturePool_) {
        destroyCachedTexture(texture);
    }
    texturePool_.clear();
}

size_t VulkanTexturePool::capacity() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return maxSize_;
}

size_t VulkanTexturePool::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return texturePool_.size();
}

size_t VulkanTexturePool::available() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t count = 0;
    for (auto& texture : texturePool_) {
        if (!texture.inUse && texture.image) count++;
    }
    return count;
}

} // namespace renderer
} // namespace heisenberg
