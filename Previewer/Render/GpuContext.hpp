//
// Created by NiceFold on 2026/6/30.
//

#pragma once

#include <memory>

namespace pl { struct gpu; }

namespace heisenberg {
namespace render {

// Phase 2: pl_gpu 的 RAII 封装（依赖 VulkanContext 创建的 VkDevice）
class GpuContext {
public:
    GpuContext();
    ~GpuContext();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace render
} // namespace heisenberg
