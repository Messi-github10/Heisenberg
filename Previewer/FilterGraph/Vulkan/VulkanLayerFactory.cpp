#include "VulkanLayerFactory.hpp"

#include "Filters/VulkanBlendLayer.hpp"
#include "Filters/VulkanColorInvertLayer.hpp"
#include "Filters/VulkanExposureLayer.hpp"
#include "Filters/VulkanGaussianBlurLayer.hpp"
#include "VulkanNodes.hpp"
#include "VulkanPipeGraph.hpp"

#include <FilterGraph/Core/BaseLayer.hpp>

namespace heisenberg::filtergraph {

VulkanLayerFactory::VulkanLayerFactory() = default;

VulkanLayerFactory::~VulkanLayerFactory() = default;

template<typename T>
T* VulkanLayerFactory::createLayer() {
    auto layer = std::make_unique<T>();
    T* result = layer.get();
    layers_.push_back(std::move(layer));
    return result;
}

IInputLayer* VulkanLayerFactory::createInput() {
    return createLayer<VulkanInputLayer>();
}

IOutputLayer* VulkanLayerFactory::createOutput() {
    return createLayer<VulkanOutputLayer>();
}

ILayer* VulkanLayerFactory::createPassthrough() {
    return createLayer<VulkanPassthroughLayer>();
}

ILayer* VulkanLayerFactory::createColorInvert() {
    return createLayer<VulkanColorInvertLayer>();
}

ITLayer<float>* VulkanLayerFactory::createExposure() {
    return createLayer<VulkanExposureLayer>();
}

ITLayer<float>* VulkanLayerFactory::createBlend() {
    return createLayer<VulkanBlendLayer>();
}

ITLayer<GaussianBlurParamet>* VulkanLayerFactory::createGaussianBlur() {
    return createLayer<VulkanGaussianBlurLayer>();
}

IPipeGraph* VulkanPipeGraphFactory::createGraph(
    const VulkanGraphContext& context) {
    return new VulkanPipeGraph(context);
}

} // namespace heisenberg::filtergraph
