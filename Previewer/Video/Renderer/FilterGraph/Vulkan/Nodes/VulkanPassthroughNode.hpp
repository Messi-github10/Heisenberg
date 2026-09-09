#pragma once

#include "VulkanNode.hpp"
#include <Video/Renderer/FilterGraph/Vulkan/Graph/LogicalResource.hpp>

namespace heisenberg::filtergraph {

class VulkanPassthroughNode final : public VulkanNode {
public:
    VulkanPassthroughNode();

    bool prepare(const VulkanGraphContext& context) override;
    void record(VkCommandBuffer commandBuffer,
                const FrameContext& frame) override;

    // Graph-owned Resource 接口
    std::vector<ResourceAccess> declareResourceAccess() const override;
    bool allocateResources(ResourceManager& manager) override;
    LogicalResourceId logicalOutputResource(int32_t index) const override;

protected:
    bool configure(const std::vector<ImageFormat>& inputs) override;

private:
    VulkanGraphContext context_;
    LogicalResourceId outputResource_;
    VkExtent2D outputExtent_ = {};
};

} // namespace heisenberg::filtergraph
