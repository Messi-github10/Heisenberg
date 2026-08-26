#pragma once

#include "Nodes/VulkanNode.hpp"
#include <FilterGraph/Interface/INodeFactory.hpp>

namespace heisenberg::filtergraph {

class VulkanOutputAdapter final : public VulkanNode, public IOutputNode {
public:
    VulkanOutputAdapter();

    void setObserver(IOutputNodeObserver* observer) override;
    bool getVulkanOutput(VulkanImageRef& image,
                         int32_t outputIndex = 0) const override;
    void releaseVulkanOutput(const VulkanImageRef& image,
                             int32_t outputIndex = 0) override;

    bool prepare(const VulkanGraphContext& context) override;
    bool beginFrame(const FrameContext& frame) override;
    void record(VkCommandBuffer commandBuffer,
                const FrameContext& frame) override;
    VulkanSyncPoint takeConsumerDone() override;

protected:
    bool configure(const std::vector<ImageFormat>& inputs) override;

private:
    IOutputNodeObserver* observer_ = nullptr;
    VulkanSyncPoint consumerDone_ = {};
};

} // namespace heisenberg::filtergraph
