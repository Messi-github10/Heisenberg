#pragma once

#include <Models/BlendMode.hpp>
#include <MultiMedia/Video/Renderer/FilterGraph/Common/FilterCommon.hpp>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>

namespace heisenberg::renderer {

struct VulkanBlendParameters {
    BlendMode mode = BlendMode::Normal;
    float opacity = 1.0f;
    // Inverse source mapping from normalized canvas coordinates. Affine
    // sampling filters premultiplied components before returning straight RGB.
    std::array<float, 4> sourceX{1, 0, 0, 0};
    std::array<float, 4> sourceY{0, 1, 0, 0};
    bool affine = false;
};

struct VulkanBlendCacheStats {
    uint64_t shaderCompilations = 0;
    uint64_t pipelineCreations = 0;
    uint64_t dispatches = 0;
};

// Worker-owned, two-input AP1 scene-linear compositor. Backdrop is opaque;
// source is straight RGBA (or premultiplied geometry preparation). The output
// is opaque RGBA16F and must not alias either input. Submissions are drained
// before returning/throwing, including cancellation. The external VkDevice
// must outlive this pass and all images supplied by its caller.
class VulkanBlendPass {
public:
    explicit VulkanBlendPass(filtergraph::VulkanGraphContext);
    ~VulkanBlendPass();
    VulkanBlendPass(const VulkanBlendPass&) = delete;
    VulkanBlendPass& operator=(const VulkanBlendPass&) = delete;
    void setCancellation(std::function<bool()>);
    filtergraph::VulkanSyncPoint execute(const filtergraph::VulkanImageRef& backdrop,
        const filtergraph::VulkanImageRef& source, const filtergraph::VulkanImageRef& output,
        const VulkanBlendParameters& = {});
    VulkanBlendCacheStats cacheStats() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace heisenberg::renderer
