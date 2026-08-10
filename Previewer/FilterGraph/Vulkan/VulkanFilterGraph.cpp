#include "VulkanFilterGraph.hpp"

#include <stdexcept>

namespace heisenberg::filtergraph {

VulkanFilterGraph::VulkanFilterGraph(const VulkanGraphContext& context) {
    VulkanPipeGraphFactory graphFactory;
    graph_.reset(graphFactory.createGraph(context));
    input_ = layerFactory_.createInput();
    colorInvert_ = layerFactory_.createColorInvert();
    exposure_ = layerFactory_.createExposure();
    blend_ = layerFactory_.createBlend();
    gaussianBlur_ = layerFactory_.createGaussianBlur();
    output_ = layerFactory_.createOutput();

    if (!graph_ || !input_ || !colorInvert_ || !exposure_ || !blend_
        || !gaussianBlur_ || !output_) {
        throw std::runtime_error("Failed to create the default Vulkan filter graph");
    }

    IBaseLayer* inputNode = graph_->addNode(input_);
    IBaseLayer* colorInvertNode = graph_->addNode(colorInvert_);
    IBaseLayer* exposureNode = graph_->addNode(exposure_);
    IBaseLayer* blendNode = graph_->addNode(blend_);
    IBaseLayer* gaussianBlurNode = graph_->addNode(gaussianBlur_);
    IBaseLayer* outputNode = graph_->addNode(output_);
    if (!inputNode || !colorInvertNode || !exposureNode || !blendNode
        || !gaussianBlurNode || !outputNode
        || !graph_->addLine(inputNode, colorInvertNode)
        || !graph_->addLine(inputNode, exposureNode)
        || !graph_->addLine(colorInvertNode, blendNode, 0, 0)
        || !graph_->addLine(exposureNode, blendNode, 0, 1)
        || !graph_->addLine(blendNode, gaussianBlurNode)
        || !graph_->addLine(gaussianBlurNode, outputNode)) {
        throw std::runtime_error("Failed to connect the default Vulkan filter graph");
    }
}

VulkanFilterGraph::~VulkanFilterGraph() = default;

} // namespace heisenberg::filtergraph
