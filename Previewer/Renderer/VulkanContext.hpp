//
// Created by NiceFold on 2026/6/30.
//

#pragma once

#include <vulkan/vulkan.hpp>
#include <Common/NonCopy.hpp>
#include <memory>

namespace heisenberg {
namespace renderer {

class VulkanContext : public NonCopy {
public:
    static VulkanContext& instance();

    ~VulkanContext() = default;
    VulkanContext(VulkanContext&&) = delete;
    VulkanContext& operator=(VulkanContext&&) = delete;

    // 初始化 Volk 加载器
    void initVolkLoader();

    // 加载 Qt 的 VkInstance 到 Volk 和 Vulkan Hpp
    void initInstance(VkInstance qtInstance);

    // 加载 Qt 的 VkDevice 到 Volk + Hpp
    void initDevice(VkDevice qtDevice);

    // 查询 Instance 级函数地址
    PFN_vkGetInstanceProcAddr getInstanceProcAddr() const;

private:
    VulkanContext();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace renderer
} // namespace heisenberg
