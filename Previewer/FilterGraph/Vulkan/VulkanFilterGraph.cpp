#include "VulkanFilterGraph.hpp"

#include <stdexcept>

namespace heisenberg::filtergraph {

VulkanFilterGraph::VulkanFilterGraph(const VulkanGraphContext& context) {
    VulkanPipeGraphFactory graphFactory;
    graph_.reset(graphFactory.createGraph(context));
    input_ = layerFactory_.createInput();
    colorInvert_ = layerFactory_.createColorInvert();
    output_ = layerFactory_.createOutput();

    if (!graph_ || !input_ || !colorInvert_ || !output_) {
        throw std::runtime_error("Failed to create the default Vulkan filter graph");
    }

    IBaseLayer* inputNode = graph_->addNode(input_);
    IBaseLayer* colorInvertNode = graph_->addNode(colorInvert_);
    IBaseLayer* outputNode = graph_->addNode(output_);
    if (!inputNode || !colorInvertNode || !outputNode
        || !graph_->addLine(inputNode, colorInvertNode)
        || !graph_->addLine(colorInvertNode, outputNode)) {
        throw std::runtime_error("Failed to connect the default Vulkan filter graph");
    }
}

VulkanFilterGraph::~VulkanFilterGraph() = default;

} // namespace heisenberg::filtergraph
