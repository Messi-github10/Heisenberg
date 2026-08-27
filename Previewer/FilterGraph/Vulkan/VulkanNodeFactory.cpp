#include "VulkanNodeFactory.hpp"
#include "VulkanInputAdapter.hpp"
#include "VulkanOutputAdapter.hpp"
#include "VulkanPassthroughNode.hpp"
#include "VulkanGraphDocument.hpp"
#include "VulkanFilterRegistry.hpp"
#include "Nodes/VulkanManifestComputeNode.hpp"
#include "Nodes/VulkanMultiPassNode.hpp"
#include "Nodes/VulkanReadbackNode.hpp"
#include <FilterGraph/Core/BaseNode.hpp>
#include <variant>

namespace heisenberg::filtergraph {
namespace {

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

} // namespace

VulkanNodeFactory::VulkanNodeFactory() = default;

VulkanNodeFactory::~VulkanNodeFactory() = default;

template<typename T, typename... Args>
T* VulkanNodeFactory::createNode(Args&&... args) {
    auto node = std::make_unique<T>(std::forward<Args>(args)...);
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

VulkanNodeCreateResult VulkanNodeFactory::createGraphNode(
    const VulkanGraphNodeDesc& node) {
    if (node.filterId == "input") return createInputNode(*this, node);
    if (node.filterId == "output") return createOutputNode(*this, node);

    std::string error;
    const VulkanFilterDescriptor* descriptor =
        VulkanFilterRegistry::instance().find(node.filterId, &error);
    if (!descriptor) return {};

    if (descriptor->kind == VulkanFilterKind::readback) {
        auto* readbackNode = createNode<VulkanReadbackNode>(*descriptor);
        return {readbackNode ? readbackNode->getNode() : nullptr,
                nullptr, nullptr};
    }
    if (descriptor->kind == VulkanFilterKind::multiPass) {
        auto* multiPassNode = createNode<VulkanMultiPassNode>(
            *descriptor, node.parameter);
        return {multiPassNode ? multiPassNode->getNode() : nullptr,
                nullptr, nullptr};
    }
    if (descriptor->kind != VulkanFilterKind::compute) return {};

    auto* manifestNode = createNode<VulkanManifestComputeNode>(*descriptor,
                                                                 node.parameter);
    return {manifestNode ? manifestNode->getNode() : nullptr, nullptr, nullptr};
}

} // namespace heisenberg::filtergraph
