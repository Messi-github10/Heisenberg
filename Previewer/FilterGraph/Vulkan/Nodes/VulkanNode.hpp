#pragma once

#include <FilterGraph/Core/BaseLayer.hpp>

#include <vector>

namespace heisenberg::filtergraph {

class VulkanGroupNode;

class VulkanNode : public BaseLayer {
public:
    VulkanNode(std::string mark, int32_t inputCount, int32_t outputCount);
    ~VulkanNode() override = default;

    void bindInputs(std::vector<VulkanImageRef> inputs);
    const VulkanImageRef& output(int32_t index) const;

    virtual bool prepare(const VulkanGraphContext& context) = 0;
    virtual bool beginFrame(const FrameContext& frame);
    virtual void record(VkCommandBuffer commandBuffer,
                        const FrameContext& frame) = 0;
    virtual void setCompletion(VulkanSyncPoint completion);
    virtual VulkanSyncPoint takeConsumerDone() { return {}; }

protected:
    const VulkanImageRef& input(int32_t index) const;
    void setOutput(int32_t index, const VulkanImageRef& image);
    void bypass();

private:
    friend class VulkanGroupNode;

    bool configureAsGroupPass(const std::vector<ImageFormat>& inputs) {
        return configure(inputs);
    }

    std::vector<VulkanImageRef> inputs_;
    std::vector<VulkanImageRef> outputs_;
};

} // namespace heisenberg::filtergraph
