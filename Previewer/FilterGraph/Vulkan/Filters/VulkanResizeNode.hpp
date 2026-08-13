#pragma once

#include "ResizeParams.hpp"
#include "VulkanComputeNode.hpp"

namespace heisenberg::filtergraph {

class VulkanResizeNode final : public VulkanComputeNode,
                               public ITNode<ResizeParams> {
public:
    VulkanResizeNode();

    IBaseNode* getNode() override {
        return static_cast<BaseNode*>(this);
    }

protected:
    bool configureOutputs(const std::vector<ImageFormat>& inputs) override;
    VulkanInputBinding inputBinding(int32_t inputIndex) const override;
    void onUpdateParamet() override;
    const char* shaderPath() const override;
};

} // namespace heisenberg::filtergraph
