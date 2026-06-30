//
// Created by NiceFold on 2026/6/30.
//

#pragma once

namespace heisenberg {
namespace render {

// Phase 2: Vulkan 实例、物理设备、逻辑设备的 RAII 封装
class VulkanContext {
public:
    VulkanContext();
    ~VulkanContext();
};

} // namespace render
} // namespace heisenberg
