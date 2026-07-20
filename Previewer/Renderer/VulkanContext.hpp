//
// Created by NiceFold on 2026/6/30.
//

#pragma once

#include <vulkan/vulkan.hpp>
#include <Common/NonCopy.hpp>
#include <memory>
#include <vector>
#include <optional>

namespace heisenberg {
namespace renderer {

class VulkanContext : public NonCopy {
public:
    static VulkanContext& instance();

    ~VulkanContext();
    VulkanContext(VulkanContext&&)      = delete;
    VulkanContext& operator=(VulkanContext&&) = delete;

    /// 第一步：初始化 Volk 加载器
    void initLoader();

    /// 第二步：创建 VkInstance
    void createInstance(bool enableValidation = false);

    /// 第三步：选择物理设备 + 创建 VkDevice
    void createDevice();

    /// 查询 Instance 级函数地址
    PFN_vkGetInstanceProcAddr getInstanceProcAddr() const;

    // -- 资源访问 --
    vk::Instance       vkInstance()          const;
    vk::PhysicalDevice physicalDevice()      const;
    vk::Device         device()              const;
    uint32_t           graphicsQueueFamily() const;
    vk::Queue          graphicsQueue()       const;

    uint32_t computeQueueFamily() const;
    vk::Queue computeQueue()      const;
    bool      hasAloneCompute()   const;
private:
    VulkanContext();

    std::vector<const char*> requiredInstanceExtensions() const;
    std::vector<const char*> requiredDeviceExtensions() const;
    std::optional<uint32_t> findGraphicsQueueFamily(vk::PhysicalDevice physDev) const;
    std::optional<uint32_t> findAloneCompute(vk::PhysicalDevice physDev) const;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace renderer
} // namespace heisenberg
