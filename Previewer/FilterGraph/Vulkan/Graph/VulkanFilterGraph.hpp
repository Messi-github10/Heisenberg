#pragma once

#include "VulkanGraphCompiler.hpp"

#include "VulkanNodeFactory.hpp"

#include <memory>

namespace heisenberg::filtergraph {

/// Owns the graph, its nodes, and the default renderer integration endpoints.
class VulkanFilterGraph {
public:
    VulkanFilterGraph(const VulkanGraphContext& context,
                      const VulkanGraphDocument& document);
    ~VulkanFilterGraph();

    VulkanFilterGraph(const VulkanFilterGraph&) = delete;
    VulkanFilterGraph& operator=(const VulkanFilterGraph&) = delete;

    IPipeGraph* graph() const { return graph_.get(); }
    VulkanNodeFactory& nodes() { return nodeFactory_; }
    IInputLayer* input() const { return input_; }
    IOutputLayer* output() const { return output_; }
    IBaseLayer* node(VulkanGraphNodeId nodeId) const;

private:
    VulkanNodeFactory nodeFactory_;
    std::unique_ptr<IPipeGraph> graph_;
    IInputLayer* input_ = nullptr;
    IOutputLayer* output_ = nullptr;
    std::unordered_map<VulkanGraphNodeId, IBaseLayer*> nodes_;
};

} // namespace heisenberg::filtergraph
