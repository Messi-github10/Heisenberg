#pragma once

#include "VulkanLayer.hpp"

#include <memory>
#include <vector>

namespace heisenberg::filtergraph {

/// A one-input, one-output layer composed of sequential internal Vulkan passes.
class VulkanGroupLayer : public VulkanLayer {
public:
    ~VulkanGroupLayer() override;

    bool prepare(const VulkanGraphContext& context) override;
    void record(VkCommandBuffer commandBuffer,
                const FrameContext& frame) override;

protected:
    explicit VulkanGroupLayer(std::string mark);

    VulkanLayer* addPass(std::unique_ptr<VulkanLayer> pass);
    bool configure(const std::vector<ImageFormat>& inputs) override;

private:
    std::vector<std::unique_ptr<VulkanLayer>> passes_;
};

} // namespace heisenberg::filtergraph
