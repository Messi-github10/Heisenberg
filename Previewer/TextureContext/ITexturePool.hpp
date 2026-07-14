//
// Created by NiceFold on 2026/7/14.
//

#pragma once

#include <vulkan/vulkan.hpp>
#include <cstddef>

namespace heisenberg {
namespace renderer {

class ITexturePool {
public:
    virtual ~ITexturePool() = default;

    /// 尝试从池中获取一个匹配规格的空闲 VkImage。
    /// @param outLayout   输出参数：VkImage 当前的 layout
    /// @param outUsage    输出参数：VkImage 的实际 usage flags
    /// @return 成功返回 VkImage，失败返回 VK_NULL_HANDLE
    virtual vk::Image tryAcquire(int width, int height, vk::Format format,
                                 vk::ImageLayout* outLayout = nullptr,
                                 VkImageUsageFlags* outUsage = nullptr) = 0;

    /// 将 VkImage 标记为空闲（Qt 已用完，可被下次 tryAcquire 复用）
    virtual void release(vk::Image image) = 0;

    /// 将新创建的 VkImage 加入池中（初始状态标记为 in-use，因为即将交给 Qt）
    /// @param layout  当前 VkImage 的 layout
    /// @param usage   VkImage 的实际 usage flags
    /// @return true 表示成功入池，false 表示池已满（调用者应自行销毁 image）
    virtual bool add(vk::Image image, int width, int height, vk::Format format,
                     vk::ImageLayout layout, VkImageUsageFlags usage) = 0;

    /// 销毁池中所有 VkImage
    virtual void drain() = 0;

    virtual size_t capacity()  const = 0;
    virtual size_t size()      const = 0;
    virtual size_t available() const = 0;
};

} // namespace renderer
} // namespace heisenberg
