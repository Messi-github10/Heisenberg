#pragma once

#include "../VulkanGroupLayer.hpp"

namespace heisenberg::filtergraph {

class VulkanGaussianBlurPassLayer;

/// Separable Gaussian blur implemented as horizontal and vertical passes.
class VulkanGaussianBlurLayer final : public VulkanGroupLayer,
                                      public ITLayer<GaussianBlurParamet> {
public:
    VulkanGaussianBlurLayer();
    ~VulkanGaussianBlurLayer() override;

    IBaseLayer* getLayer() override {
        return static_cast<BaseLayer*>(this);
    }

protected:
    void onUpdateParamet() override;

private:
    void updatePassParameters();

    VulkanGaussianBlurPassLayer* horizontalPass_ = nullptr;
    VulkanGaussianBlurPassLayer* verticalPass_ = nullptr;
};

} // namespace heisenberg::filtergraph
