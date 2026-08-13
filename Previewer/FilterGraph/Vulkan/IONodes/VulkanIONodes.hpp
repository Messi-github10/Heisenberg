#pragma once

#include "VulkanNode.hpp"
#include <FilterGraph/Interface/INodeFactory.hpp>

namespace heisenberg::filtergraph {

class VulkanInputNode final : public VulkanNode, public IInputNode {
public:
    VulkanInputNode();

    void setImage(const ImageFormat& format) override;
    void setImage(const VideoFormat& format) override;
    bool setVulkanInput(const VulkanImageRef& image,
                        int32_t inputIndex = 0) override;

    bool prepare(const VulkanGraphContext& context) override;
    bool beginFrame(const FrameContext& frame) override;
    void record(VkCommandBuffer commandBuffer,
                const FrameContext& frame) override;

protected:
    bool configure(const std::vector<ImageFormat>& inputs) override;

private:
    ImageFormat declaredFormat_ = {};
    VulkanImageRef externalImage_ = {};
};

class VulkanOutputNode final : public VulkanNode, public IOutputNode {
public:
    VulkanOutputNode();

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
