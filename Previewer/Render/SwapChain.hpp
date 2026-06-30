//
// Created by NiceFold on 2026/6/30.
//

#pragma once

#include <memory>

namespace heisenberg {
namespace render {

// Phase 2: Swapchain，封装 pl_swapchain，管理窗口表面和帧呈现
class SwapChain {
public:
    SwapChain();
    ~SwapChain();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace render
} // namespace heisenberg
