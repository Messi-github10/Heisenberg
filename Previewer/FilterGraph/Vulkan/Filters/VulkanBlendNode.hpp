#pragma once

#include "VulkanComputeNode.hpp"

namespace heisenberg::filtergraph {

/// Linear crossfade between two images with identical working formats.
class VulkanBlendNode final : public VulkanComputeNode,
                               public ITLayer<float> {
public:
    VulkanBlendNode();

    IBaseLayer* getLayer() override {
        return static_cast<BaseLayer*>(this);
    }

protected:
    bool configureOutputs(const std::vector<ImageFormat>& inputs) override;
    void onUpdateParamet() override;
    const char* shaderPath() const override;
};

} // namespace heisenberg::filtergraph
