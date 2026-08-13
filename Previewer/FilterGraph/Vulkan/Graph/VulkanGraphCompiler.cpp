#include "VulkanGraphCompiler.hpp"
#include "VulkanNodeFactory.hpp"
#include <FilterGraph/Interface/IPipeGraph.hpp>
#include <utility>

namespace heisenberg::filtergraph {
namespace {

void setError(std::string* error, const char* message) {
    if (error) *error = message;
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
        const VulkanNodeCreateResult runtimeNode =
            nodeFactory.createGraphNode(node);
        if (!runtimeNode.node || !graph.addNode(runtimeNode.node)) {
            setError(error, "Failed to create or add a Vulkan graph node");
            return false;
        }
        if (runtimeNode.input) compiled.input = runtimeNode.input;
        if (runtimeNode.output) compiled.output = runtimeNode.output;
        compiled.nodes.emplace(node.id, runtimeNode.node);
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
