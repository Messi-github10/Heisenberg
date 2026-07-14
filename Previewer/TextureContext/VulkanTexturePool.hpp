//
// Created by NiceFold on 2026/7/14.
//

#pragma once

#include "ITexturePool.hpp"

#include <vector>
#include <mutex>

#ifndef HEISENBERG_TEXTURE_POOL_SIZE
#define HEISENBERG_TEXTURE_POOL_SIZE 3
#endif

namespace heisenberg {
namespace renderer {

class VulkanTexturePool : public ITexturePool {
public:
    VulkanTexturePool(vk::Device device, size_t maxSize = HEISENBERG_TEXTURE_POOL_SIZE);
    ~VulkanTexturePool() override;

    vk::Image tryAcquire(int width, int height, vk::Format format,
                         vk::ImageLayout* outLayout = nullptr,
                         VkImageUsageFlags* outUsage = nullptr) override;
    void      release(vk::Image image) override;
    bool      add(vk::Image image, int width, int height, vk::Format format,
                  vk::ImageLayout layout, VkImageUsageFlags usage) override;
    void      drain() override;

    size_t capacity()  const override;
    size_t size()      const override;
    size_t available() const override;

private:
    struct CachedTexture {
        vk::Image            image         = nullptr;
        int                  width         = 0;
        int                  height        = 0;
        vk::Format           format        = vk::Format::eUndefined;
        vk::ImageLayout      currentLayout = vk::ImageLayout::eUndefined;
        VkImageUsageFlags    usageFlags    = 0;
        bool                 inUse         = false;
    };

    void destroyCachedTexture(CachedTexture& cachedTexture);

    std::vector<CachedTexture> texturePool_;
    size_t             maxSize_;
    vk::Device         device_;
    mutable std::mutex mutex_;
};

} // namespace renderer
} // namespace heisenberg
