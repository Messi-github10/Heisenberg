#include "VulkanNodeFactory.hpp"
#include "VulkanBlendNode.hpp"
#include "VulkanColorInvertNode.hpp"
#include "VulkanExposureNode.hpp"
#include "VulkanGaussianBlurNode.hpp"
#include "VulkanIONodes.hpp"
#include "VulkanPassthroughNode.hpp"
#include <FilterGraph/Core/BaseNode.hpp>

namespace heisenberg::filtergraph {

VulkanNodeFactory::VulkanNodeFactory() = default;

VulkanNodeFactory::~VulkanNodeFactory() = default;

template<typename T>
T* VulkanNodeFactory::createNode() {
    auto node = std::make_unique<T>();
    T* result = node.get();
    nodes_.push_back(std::move(node));
    return result;
}

IInputNode* VulkanNodeFactory::createInput() {
    return createNode<VulkanInputNode>();
}

IOutputNode* VulkanNodeFactory::createOutput() {
    return createNode<VulkanOutputNode>();
}

INode* VulkanNodeFactory::createPassthrough() {
    return createNode<VulkanPassthroughNode>();
}

INode* VulkanNodeFactory::createColorInvert() {
    return createNode<VulkanColorInvertNode>();
}

ITNode<float>* VulkanNodeFactory::createExposure() {
    return createNode<VulkanExposureNode>();
}

ITNode<float>* VulkanNodeFactory::createBlend() {
    return createNode<VulkanBlendNode>();
}

ITNode<GaussianBlurParams>* VulkanNodeFactory::createGaussianBlur() {
    return createNode<VulkanGaussianBlurNode>();
}

} // namespace heisenberg::filtergraph
