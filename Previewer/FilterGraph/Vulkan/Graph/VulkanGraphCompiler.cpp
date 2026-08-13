#include "VulkanGraphCompiler.hpp"
#include "VulkanNodeFactory.hpp"
#include <FilterGraph/Interface/INodeFactory.hpp>
#include <FilterGraph/Interface/IPipeGraph.hpp>
#include <utility>
#include <variant>

namespace heisenberg::filtergraph {
namespace {

void setError(std::string* error, const char* message) {
    if (error) *error = message;
}

IBaseNode* createRuntimeNode(
    const VulkanGraphNodeDesc& node,
    VulkanNodeFactory& nodeFactory,
    IInputNode*& input,
    IOutputNode*& output) {
    switch (node.type) {
        case VulkanGraphNodeType::input:
            input = nodeFactory.createInput();
            return input ? input->getNode() : nullptr;
        case VulkanGraphNodeType::output:
            output = nodeFactory.createOutput();
            return output ? output->getNode() : nullptr;
        case VulkanGraphNodeType::colorInvert: {
            INode* runtimeNode = nodeFactory.createColorInvert();
            return runtimeNode ? runtimeNode->getNode() : nullptr;
        }
        case VulkanGraphNodeType::exposure: {
            auto* runtimeNode = nodeFactory.createExposure();
            if (!runtimeNode) return nullptr;
            runtimeNode->updateParamet(std::get<ExposureParamet>(node.parameter).exposure);
            return runtimeNode->getNode();
        }
        case VulkanGraphNodeType::blend: {
            auto* runtimeNode = nodeFactory.createBlend();
            if (!runtimeNode) return nullptr;
            runtimeNode->updateParamet(std::get<BlendParamet>(node.parameter).factor);
            return runtimeNode->getNode();
        }
        case VulkanGraphNodeType::gaussianBlur: {
            auto* runtimeNode = nodeFactory.createGaussianBlur();
            if (!runtimeNode) return nullptr;
            runtimeNode->updateParamet(
                std::get<GaussianBlurParams>(node.parameter));
            return runtimeNode->getNode();
        }
    }
    return nullptr;
}

} // namespace

bool VulkanGraphCompiler::compile(
    const VulkanGraphDocument& document,
    VulkanNodeFactory& nodeFactory,
    IPipeGraph& graph,
    VulkanCompiledGraph& result,
    std::string* error) {
    if (!document.validate(error)) return false;

    VulkanCompiledGraph compiled;
    compiled.nodes.reserve(document.nodes().size());
    for (const VulkanGraphNodeDesc& node : document.nodes()) {
        IBaseNode* runtimeNode = createRuntimeNode(
            node, nodeFactory, compiled.input, compiled.output);
        if (!runtimeNode || !graph.addNode(runtimeNode)) {
            setError(error, "Failed to create or add a Vulkan graph node");
            return false;
        }
        compiled.nodes.emplace(node.id, runtimeNode);
    }

    for (const GraphEdge& edge : document.edges()) {
        const auto source = compiled.nodes.find(edge.output.nodeId);
        const auto destination = compiled.nodes.find(edge.input.nodeId);
        if (source == compiled.nodes.end()
            || destination == compiled.nodes.end()
            || !graph.addLine(source->second, destination->second,
                              edge.output.pinIndex, edge.input.pinIndex)) {
            setError(error, "Failed to connect a Vulkan graph edge");
            return false;
        }
    }

    if (!compiled.input || !compiled.output) {
        setError(error, "Compiled Vulkan graph has no input or output");
        return false;
    }
    result = std::move(compiled);
    return true;
}

} // namespace heisenberg::filtergraph
