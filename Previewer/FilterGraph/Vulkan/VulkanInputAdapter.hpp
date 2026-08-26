#pragma once

#include "Nodes/VulkanNode.hpp"
#include <FilterGraph/Interface/INodeFactory.hpp>

namespace heisenberg::filtergraph {

class VulkanInputAdapter final : public VulkanNode, public IInputNode {
public:
    VulkanInputAdapter();

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

} // namespace heisenberg::filtergraph
