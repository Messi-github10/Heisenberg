#pragma once

#include "VulkanComputeNode.hpp"

namespace heisenberg::filtergraph {

class VulkanExposureNode final : public VulkanComputeNode,
                                  public ITNode<float> {
public:
    VulkanExposureNode();

    IBaseNode* getNode() override {
        return static_cast<BaseNode*>(this);
    }

protected:
    void onUpdateParamet() override;
    const char* shaderPath() const override;
};

} // namespace heisenberg::filtergraph
