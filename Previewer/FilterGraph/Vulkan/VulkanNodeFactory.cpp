#include "VulkanNodeFactory.hpp"
#include "VulkanBlendNode.hpp"
#include "VulkanColorInvertNode.hpp"
#include "VulkanExposureNode.hpp"
#include "VulkanGaussianBlurNode.hpp"
#include "VulkanHistogramNode.hpp"
#include "VulkanInputAdapter.hpp"
#include "VulkanOutputAdapter.hpp"
#include "VulkanLutNode.hpp"
#include "VulkanPassthroughNode.hpp"
#include "VulkanResizeNode.hpp"
#include "VulkanGraphDocument.hpp"
#include <FilterGraph/Core/BaseNode.hpp>
#include <unordered_map>
#include <variant>

namespace heisenberg::filtergraph {
namespace {

using GraphNodeCreator = VulkanNodeCreateResult (*) (
    VulkanNodeFactory&, const VulkanGraphNodeDesc&);

VulkanNodeCreateResult createInputNode(
    VulkanNodeFactory& factory, const VulkanGraphNodeDesc&) {
    IInputNode* input = factory.createInput();
    return {input ? input->getNode() : nullptr, input, nullptr};
}

VulkanNodeCreateResult createOutputNode(
    VulkanNodeFactory& factory, const VulkanGraphNodeDesc&) {
    IOutputNode* output = factory.createOutput();
    return {output ? output->getNode() : nullptr, nullptr, output};
}

VulkanNodeCreateResult createColorInvertNode(
    VulkanNodeFactory& factory, const VulkanGraphNodeDesc&) {
    IFilterNode* node = factory.createColorInvert();
    return {node ? node->getNode() : nullptr, nullptr, nullptr};
}

VulkanNodeCreateResult createExposureNode(
    VulkanNodeFactory& factory, const VulkanGraphNodeDesc& descriptor) {
    auto* node = factory.createExposure();
    if (!node) return {};
    node->updateParamet(std::get<ExposureParamet>(descriptor.parameter).exposure);
    return {node->getNode(), nullptr, nullptr};
}

VulkanNodeCreateResult createBlendNode(
    VulkanNodeFactory& factory, const VulkanGraphNodeDesc& descriptor) {
    auto* node = factory.createBlend();
    if (!node) return {};
    node->updateParamet(std::get<BlendParamet>(descriptor.parameter).factor);
    return {node->getNode(), nullptr, nullptr};
}

VulkanNodeCreateResult createGaussianBlurNode(
    VulkanNodeFactory& factory, const VulkanGraphNodeDesc& descriptor) {
    auto* node = factory.createGaussianBlur();
    if (!node) return {};
    node->updateParamet(std::get<GaussianBlurParams>(descriptor.parameter));
    return {node->getNode(), nullptr, nullptr};
}

VulkanNodeCreateResult createResizeNode(
    VulkanNodeFactory& factory, const VulkanGraphNodeDesc& descriptor) {
    auto* node = factory.createResize();
    if (!node) return {};
    node->updateParamet(std::get<ResizeParams>(descriptor.parameter));
    return {node->getNode(), nullptr, nullptr};
}

VulkanNodeCreateResult createLutNode(
    VulkanNodeFactory& factory, const VulkanGraphNodeDesc&) {
    IFilterNode* node = factory.createLut();
    return {node ? node->getNode() : nullptr, nullptr, nullptr};
}

VulkanNodeCreateResult createHistogramNode(
    VulkanNodeFactory& factory, const VulkanGraphNodeDesc&) {
    IFilterNode* node = factory.createHistogram();
    return {node ? node->getNode() : nullptr, nullptr, nullptr};
}

const std::unordered_map<VulkanGraphNodeType, GraphNodeCreator>& graphNodeCreators() {
    static const std::unordered_map<VulkanGraphNodeType, GraphNodeCreator> creators{
        {VulkanGraphNodeType::input, createInputNode},
        {VulkanGraphNodeType::output, createOutputNode},
        {VulkanGraphNodeType::colorInvert, createColorInvertNode},
        {VulkanGraphNodeType::exposure, createExposureNode},
        {VulkanGraphNodeType::blend, createBlendNode},
        {VulkanGraphNodeType::gaussianBlur, createGaussianBlurNode},
        {VulkanGraphNodeType::resize, createResizeNode},
        {VulkanGraphNodeType::lut, createLutNode},
        {VulkanGraphNodeType::histogram, createHistogramNode},
    };
    return creators;
}

} // namespace

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
    return createNode<VulkanInputAdapter>();
}

IOutputNode* VulkanNodeFactory::createOutput() {
    return createNode<VulkanOutputAdapter>();
}

IFilterNode* VulkanNodeFactory::createPassthrough() {
    return createNode<VulkanPassthroughNode>();
}

IFilterNode* VulkanNodeFactory::createColorInvert() {
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

ITNode<ResizeParams>* VulkanNodeFactory::createResize() {
    return createNode<VulkanResizeNode>();
}

IFilterNode* VulkanNodeFactory::createLut() {
    return createNode<VulkanLutNode>();
}

IFilterNode* VulkanNodeFactory::createHistogram() {
    return createNode<VulkanHistogramNode>();
}

VulkanNodeCreateResult VulkanNodeFactory::createGraphNode(
    const VulkanGraphNodeDesc& node) {
    const auto& creators = graphNodeCreators();
    const auto creator = creators.find(node.type);
    return creator != creators.end() ? creator->second(*this, node)
                                    : VulkanNodeCreateResult{};
}

} // namespace heisenberg::filtergraph
