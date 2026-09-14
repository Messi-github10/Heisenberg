#pragma once

#include "VulkanNode.hpp"
#include <memory>
#include <vector>

namespace heisenberg::filtergraph {

class VulkanGroupNode : public VulkanNode {
public:
    ~VulkanGroupNode() override;

    bool prepare(const VulkanGraphContext& context) override;
    void record(VkCommandBuffer commandBuffer,
                const FrameContext& frame) override;
    void setCompletion(VulkanSyncPoint completion) override;

    // Graph-owned Resource 接口
    std::vector<LogicalResourceRequest> declareResourceRequests() const override;
    std::vector<ResourceAccess> declareResourceAccess() const override;
    void bindDeclaredResources(
        const std::vector<LogicalResourceId>& resources) override;
    LogicalResourceId logicalOutputResource(int32_t index) const override;

protected:
    explicit VulkanGroupNode(std::string mark);

    VulkanNode* addPass(std::unique_ptr<VulkanNode> pass);
    bool configure(const std::vector<ImageFormat>& inputs) override;

private:
    std::vector<std::unique_ptr<VulkanNode>> passes_;
};

} // namespace heisenberg::filtergraph
