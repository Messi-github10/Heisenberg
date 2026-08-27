#include "VulkanFilterGraph.hpp"

#include "VulkanPipeGraph.hpp"
#include "../Nodes/VulkanManifestComputeNode.hpp"
#include "../Nodes/VulkanReadbackNode.hpp"

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

bool VulkanFilterGraph::setLutImage(
    VulkanGraphNodeId nodeId, const VulkanImageRef& image) {
    auto* lut = dynamic_cast<VulkanManifestComputeNode*>(node(nodeId));
    return lut && lut->setExternalInput(0, image);
}

bool VulkanFilterGraph::histogramBins(
    VulkanGraphNodeId nodeId, std::array<uint32_t, 256>& bins) const {
    auto* readback = dynamic_cast<VulkanReadbackNode*>(node(nodeId));
    return readback
        && readback->readback(bins.data(), bins.size() * sizeof(uint32_t));
}

} // namespace heisenberg::filtergraph
