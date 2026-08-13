#pragma once

#include "VulkanComputeNode.hpp"

namespace heisenberg::filtergraph {

class VulkanBlendNode final : public VulkanComputeNode,
                               public ITNode<float> {
public:
    VulkanBlendNode();

    IBaseNode* getNode() override {
        return static_cast<BaseNode*>(this);
    }

protected:
    bool configureOutputs(const std::vector<ImageFormat>& inputs) override;
    void onUpdateParamet() override;
    const char* shaderPath() const override;
};

} // namespace heisenberg::filtergraph
