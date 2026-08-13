#pragma once

#include <FilterGraph/Interface/INodeFactory.hpp>
#include "GaussianBlurParams.hpp"
#include <memory>
#include <vector>

namespace heisenberg::filtergraph {

class BaseNode;

class VulkanNodeFactory final : public NodeFactory {
public:
    VulkanNodeFactory();
    ~VulkanNodeFactory() override;

    IInputNode* createInput() override;
    IOutputNode* createOutput() override;
    INode* createPassthrough() override;
    INode* createColorInvert() override;
    ITNode<float>* createExposure() override;
    ITNode<float>* createBlend() override;
    ITNode<GaussianBlurParams>* createGaussianBlur() override;

private:
    template<typename T>
    T* createNode();

    std::vector<std::unique_ptr<BaseNode>> nodes_;
};

} // namespace heisenberg::filtergraph
