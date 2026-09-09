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

protected:
    explicit VulkanGroupNode(std::string mark);

    VulkanNode* addPass(std::unique_ptr<VulkanNode> pass);
    bool configure(const std::vector<ImageFormat>& inputs) override;

private:
    std::vector<std::unique_ptr<VulkanNode>> passes_;
};

} // namespace heisenberg::filtergraph
