#pragma once

#include "VulkanLayerFactory.hpp"

#include <memory>

namespace heisenberg::filtergraph {

/// Owns the graph, its layers, and the default renderer integration endpoints.
class VulkanFilterGraph {
public:
    explicit VulkanFilterGraph(const VulkanGraphContext& context);
    ~VulkanFilterGraph();

    VulkanFilterGraph(const VulkanFilterGraph&) = delete;
    VulkanFilterGraph& operator=(const VulkanFilterGraph&) = delete;

    IPipeGraph* graph() const { return graph_.get(); }
    VulkanLayerFactory& layers() { return layerFactory_; }
    IInputLayer* input() const { return input_; }
    IOutputLayer* output() const { return output_; }
    ILayer* colorInvert() const { return colorInvert_; }

private:
    VulkanLayerFactory layerFactory_;
    std::unique_ptr<IPipeGraph> graph_;
    IInputLayer* input_ = nullptr;
    IOutputLayer* output_ = nullptr;
    ILayer* colorInvert_ = nullptr;
};

} // namespace heisenberg::filtergraph
