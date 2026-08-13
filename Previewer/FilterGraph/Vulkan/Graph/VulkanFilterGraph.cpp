#include "VulkanFilterGraph.hpp"

#include "VulkanPipeGraph.hpp"

#include <stdexcept>
#include <utility>

namespace heisenberg::filtergraph {

VulkanFilterGraph::VulkanFilterGraph(
    const VulkanGraphContext& context,
    const VulkanGraphDocument& document)
    : graph_(std::make_unique<VulkanPipeGraph>(context)) {
    VulkanCompiledGraph compiled;
    std::string error;
    if (!VulkanGraphCompiler::compile(
            document, nodeFactory_, *graph_, compiled, &error)) {
        throw std::runtime_error(
            "Failed to compile Vulkan filter graph: " + error);
    }
    input_ = compiled.input;
    output_ = compiled.output;
    nodes_ = std::move(compiled.nodes);
}

VulkanFilterGraph::~VulkanFilterGraph() = default;

IBaseNode* VulkanFilterGraph::node(VulkanGraphNodeId nodeId) const {
    const auto found = nodes_.find(nodeId);
    return found == nodes_.end() ? nullptr : found->second;
}

} // namespace heisenberg::filtergraph
