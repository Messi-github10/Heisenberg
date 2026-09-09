#pragma once

#include "VulkanGraphCompiler.hpp"

#include "VulkanNodeFactory.hpp"

#include <array>
#include <memory>

namespace heisenberg::filtergraph {

class VulkanFilterGraph {
public:
    VulkanFilterGraph(const VulkanGraphContext& context,
                      const VulkanGraphDocument& document);
    ~VulkanFilterGraph();

    VulkanFilterGraph(const VulkanFilterGraph&) = delete;
    VulkanFilterGraph& operator=(const VulkanFilterGraph&) = delete;

    IPipeGraph* graph() const { return graph_.get(); }
    VulkanNodeFactory& nodes() { return nodeFactory_; }
    IInputNode* input() const { return input_; }
    IOutputNode* output() const { return output_; }
    IBaseNode* node(VulkanGraphNodeId nodeId) const;
    bool setLutImage(VulkanGraphNodeId nodeId,
                     const VulkanImageRef& image);
    bool histogramBins(VulkanGraphNodeId nodeId,
                       std::array<uint32_t, 256>& bins) const;

private:
    VulkanNodeFactory nodeFactory_;
    std::unique_ptr<IPipeGraph> graph_;
    IInputNode* input_ = nullptr;
    IOutputNode* output_ = nullptr;
    std::unordered_map<VulkanGraphNodeId, IBaseNode*> nodes_;
};

} // namespace heisenberg::filtergraph
