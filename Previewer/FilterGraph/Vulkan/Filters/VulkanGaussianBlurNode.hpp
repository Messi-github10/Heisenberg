#pragma once

#include "VulkanGroupNode.hpp"
#include "GaussianBlurParams.hpp"

namespace heisenberg::filtergraph {

class VulkanGaussianBlurPassNode;

class VulkanGaussianBlurNode final : public VulkanGroupNode,
                                      public ITNode<GaussianBlurParams> {
public:
    VulkanGaussianBlurNode();
    ~VulkanGaussianBlurNode() override;

    IBaseNode* getNode() override {
        return static_cast<BaseNode*>(this);
    }

protected:
    void onUpdateParamet() override;

private:
    void updatePassParameters();

    VulkanGaussianBlurPassNode* horizontalPass_ = nullptr;
    VulkanGaussianBlurPassNode* verticalPass_ = nullptr;
};

} // namespace heisenberg::filtergraph
