//
// Created by NiceFold on 2026/6/30.
//

#pragma once

#include <memory>

namespace heisenberg {
namespace render {

// Phase 2: 渲染引擎，封装 pl_renderer，执行色彩处理
class RenderEngine {
public:
    RenderEngine();
    ~RenderEngine();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace render
} // namespace heisenberg
