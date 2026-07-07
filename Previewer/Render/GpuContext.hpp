//
// Created by NiceFold on 2026/6/30.
//

#pragma once

#include <memory>

extern "C" {
#include <libplacebo/vulkan.h>
}

namespace heisenberg {
namespace render {

class GpuContext {
public:
    GpuContext();
    ~GpuContext();

    GpuContext(const GpuContext&)            = delete;
    GpuContext& operator=(const GpuContext&) = delete;

    pl_gpu    gpu()    const;
    pl_vulkan vulkan() const;

private:
    void createLog();
    void importVulkan();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace render
} // namespace heisenberg
