#pragma once

#include "VulkanGroupNode.hpp"

namespace heisenberg::filtergraph {

class VulkanGaussianBlurPassNode;

/// Separable Gaussian blur implemented as horizontal and vertical passes.
class VulkanGaussianBlurNode final : public VulkanGroupNode,
                                      public ITLayer<GaussianBlurParamet> {
public:
    VulkanGaussianBlurNode();
    ~VulkanGaussianBlurNode() override;

    IBaseLayer* getLayer() override {
        return static_cast<BaseLayer*>(this);
    }

protected:
    void onUpdateParamet() override;

private:
    void updatePassParameters();

    VulkanGaussianBlurPassNode* horizontalPass_ = nullptr;
    VulkanGaussianBlurPassNode* verticalPass_ = nullptr;
};

} // namespace heisenberg::filtergraph
