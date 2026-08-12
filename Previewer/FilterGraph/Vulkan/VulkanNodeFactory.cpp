#include "VulkanNodeFactory.hpp"

#include "VulkanBlendNode.hpp"
#include "VulkanColorInvertNode.hpp"
#include "VulkanExposureNode.hpp"
#include "VulkanGaussianBlurNode.hpp"
#include "VulkanIOLayer.hpp"
#include "VulkanPassthroughNode.hpp"

#include <FilterGraph/Core/BaseLayer.hpp>

namespace heisenberg::filtergraph {

VulkanNodeFactory::VulkanNodeFactory() = default;

VulkanNodeFactory::~VulkanNodeFactory() = default;

template<typename T>
T* VulkanNodeFactory::createNode() {
    auto layer = std::make_unique<T>();
    T* result = layer.get();
    nodes_.push_back(std::move(layer));
    return result;
}

IInputLayer* VulkanNodeFactory::createInput() {
    return createNode<VulkanInputLayer>();
}

IOutputLayer* VulkanNodeFactory::createOutput() {
    return createNode<VulkanOutputLayer>();
}

ILayer* VulkanNodeFactory::createPassthrough() {
    return createNode<VulkanPassthroughNode>();
}

ILayer* VulkanNodeFactory::createColorInvert() {
    return createNode<VulkanColorInvertNode>();
}

ITLayer<float>* VulkanNodeFactory::createExposure() {
    return createNode<VulkanExposureNode>();
}

ITLayer<float>* VulkanNodeFactory::createBlend() {
    return createNode<VulkanBlendNode>();
}

ITLayer<GaussianBlurParamet>* VulkanNodeFactory::createGaussianBlur() {
    return createNode<VulkanGaussianBlurNode>();
}

} // namespace heisenberg::filtergraph
