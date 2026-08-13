#pragma once

#include <FilterGraph/Interface/INodeFactory.hpp>
#include "GaussianBlurParams.hpp"
#include "ResizeParams.hpp"
#include <memory>
#include <vector>

namespace heisenberg::filtergraph {

class BaseNode;
class IBaseNode;
class IInputNode;
class IOutputNode;
struct VulkanGraphNodeDesc;

struct VulkanNodeCreateResult {
    IBaseNode* node = nullptr;
    IInputNode* input = nullptr;
    IOutputNode* output = nullptr;
};

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
    ITNode<ResizeParams>* createResize() override;
    INode* createLut() override;
    INode* createHistogram() override;

    VulkanNodeCreateResult createGraphNode(const VulkanGraphNodeDesc& node);

private:
    template<typename T>
    T* createNode();

    std::vector<std::unique_ptr<BaseNode>> nodes_;
};

} // namespace heisenberg::filtergraph
