#pragma once

#include "VulkanComputeNode.hpp"

namespace heisenberg::filtergraph {

class VulkanExposureNode final : public VulkanComputeNode,
                                  public ITLayer<float> {
public:
    VulkanExposureNode();

    IBaseLayer* getLayer() override {
        return static_cast<BaseLayer*>(this);
    }

protected:
    void onUpdateParamet() override;
    const char* shaderPath() const override;
};

} // namespace heisenberg::filtergraph
