#pragma once

#include "VulkanImageResource.hpp"
#include "VulkanNode.hpp"
#include <memory>

namespace heisenberg::filtergraph {

class VulkanPassthroughNode final : public VulkanNode {
public:
    VulkanPassthroughNode();

    bool prepare(const VulkanGraphContext& context) override;
    void record(VkCommandBuffer commandBuffer,
                const FrameContext& frame) override;

protected:
    bool configure(const std::vector<ImageFormat>& inputs) override;

private:
    std::unique_ptr<VulkanImageResource> outputImage_;
};

} // namespace heisenberg::filtergraph
