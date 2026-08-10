#pragma once

#include "../VulkanComputeLayer.hpp"

namespace heisenberg::filtergraph {

/// Linear crossfade between two images with identical working formats.
class VulkanBlendLayer final : public VulkanComputeLayer,
                               public ITLayer<float> {
public:
    VulkanBlendLayer();

    IBaseLayer* getLayer() override {
        return static_cast<BaseLayer*>(this);
    }

protected:
    bool configureOutputs(const std::vector<ImageFormat>& inputs) override;
    void onUpdateParamet() override;
    const char* shaderPath() const override;
};

} // namespace heisenberg::filtergraph
