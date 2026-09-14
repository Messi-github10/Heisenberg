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
    std::vector<LogicalResourceRequest> declareResourceRequests() const override;
    std::vector<ResourceAccess> declareResourceAccess() const override;

protected:
    bool configure(const std::vector<ImageFormat>& inputs) override;

private:
    VulkanGraphContext context_;
    VkExtent2D outputExtent_ = {};
};

} // namespace heisenberg::filtergraph
