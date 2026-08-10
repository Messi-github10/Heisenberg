#pragma once

#include "../VulkanComputeLayer.hpp"

namespace heisenberg::filtergraph {

class VulkanExposureLayer final : public VulkanComputeLayer,
                                  public ITLayer<float> {
public:
    VulkanExposureLayer();

    IBaseLayer* getLayer() override {
        return static_cast<BaseLayer*>(this);
    }

protected:
    void onUpdateParamet() override;
    const char* shaderPath() const override;
};

} // namespace heisenberg::filtergraph
