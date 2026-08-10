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
    ITLayer<float>* exposure() const { return exposure_; }
    ITLayer<float>* blend() const { return blend_; }
    ITLayer<GaussianBlurParamet>* gaussianBlur() const {
        return gaussianBlur_;
    }

private:
    VulkanLayerFactory layerFactory_;
    std::unique_ptr<IPipeGraph> graph_;
    IInputLayer* input_ = nullptr;
    IOutputLayer* output_ = nullptr;
    ILayer* colorInvert_ = nullptr;
    ITLayer<float>* exposure_ = nullptr;
    ITLayer<float>* blend_ = nullptr;
    ITLayer<GaussianBlurParamet>* gaussianBlur_ = nullptr;
};

} // namespace heisenberg::filtergraph
