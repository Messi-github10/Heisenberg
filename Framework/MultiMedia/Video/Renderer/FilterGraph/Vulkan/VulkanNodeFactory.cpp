#include "VulkanNodeFactory.hpp"
#include "VulkanInputAdapter.hpp"
#include "VulkanOutputAdapter.hpp"
#include "VulkanGraphDocument.hpp"
#include "VulkanFilterRegistry.hpp"
#include "Nodes/VulkanManifestComputeNode.hpp"
#include "Nodes/VulkanMultiPassNode.hpp"
#include "Nodes/VulkanProgramNode.hpp"
#include "Nodes/VulkanReadbackNode.hpp"
#include <MultiMedia/Video/Renderer/FilterGraph/Core/BaseNode.hpp>
#include <variant>

namespace heisenberg::filtergraph {
namespace {

VulkanNodeCreateResult createInputNode(
    VulkanNodeFactory& factory, const VulkanGraphNodeDesc& node) {
    IInputNode* input = factory.createInput();
    if (auto* adapter = dynamic_cast<VulkanInputAdapter*>(input))
        adapter->setImageContracts(node.inputContract, node.outputContract);
    return {input ? input->getNode() : nullptr, input, nullptr};
}

VulkanNodeCreateResult createOutputNode(
    VulkanNodeFactory& factory, const VulkanGraphNodeDesc& node) {
    IOutputNode* output = factory.createOutput();
    if (auto* adapter = dynamic_cast<VulkanOutputAdapter*>(output))
        adapter->setImageContracts(node.inputContract, node.outputContract);
    return {output ? output->getNode() : nullptr, nullptr, output};
}

} // namespace

VulkanNodeFactory::VulkanNodeFactory(GraphImageContract imageContract)
    : imageContract_(imageContract) {}

VulkanNodeFactory::~VulkanNodeFactory() = default;

template<typename T, typename... Args>
T* VulkanNodeFactory::createNode(Args&&... args) {
    auto node = std::make_unique<T>(std::forward<Args>(args)...);
    T* result = node.get();
    nodes_.push_back(std::move(node));
    return result;
}

IInputNode* VulkanNodeFactory::createInput() {
    return createNode<VulkanInputAdapter>(imageContract_);
}

IOutputNode* VulkanNodeFactory::createOutput() {
    return createNode<VulkanOutputAdapter>(imageContract_);
}

VulkanNodeCreateResult VulkanNodeFactory::createGraphNode(
    const VulkanGraphNodeDesc& node) {
    if (node.filterId == "input") return createInputNode(*this, node);
    if (node.filterId == "output") return createOutputNode(*this, node);
    if (node.filterId == "program") {
        if (!node.program || !node.inputContract || !node.outputContract) return {};
        auto* program = createNode<VulkanProgramNode>(node.program,
            *node.inputContract, *node.outputContract);
        return {program->getNode(), nullptr, nullptr};
    }

    std::string error;
    const VulkanFilterDescriptor* descriptor =
        VulkanFilterRegistry::instance().find(node.filterId, &error);
    if (!descriptor) return {};

    if (descriptor->kind == VulkanFilterKind::readback) {
        auto* readbackNode = createNode<VulkanReadbackNode>(*descriptor);
        readbackNode->setImageContracts(node.inputContract, node.outputContract);
        return {readbackNode ? readbackNode->getNode() : nullptr,
                nullptr, nullptr};
    }
    if (descriptor->kind == VulkanFilterKind::multiPass) {
        auto* multiPassNode = createNode<VulkanMultiPassNode>(
            *descriptor, node.parameter);
        multiPassNode->setImageContracts(node.inputContract, node.outputContract);
        return {multiPassNode ? multiPassNode->getNode() : nullptr,
                nullptr, nullptr};
    }
    if (descriptor->kind != VulkanFilterKind::compute) return {};

    auto* manifestNode = createNode<VulkanManifestComputeNode>(*descriptor,
                                                                 node.parameter);
    manifestNode->setImageContracts(node.inputContract, node.outputContract);
    return {manifestNode ? manifestNode->getNode() : nullptr, nullptr, nullptr};
}

} // namespace heisenberg::filtergraph
