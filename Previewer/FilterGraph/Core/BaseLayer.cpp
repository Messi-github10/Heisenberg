#include "BaseLayer.hpp"

#include "PipeGraph.hpp"

#include <stdexcept>
#include <utility>

namespace heisenberg::filtergraph {

BaseLayer::BaseLayer(std::string mark, int32_t inputCount, int32_t outputCount)
    : mark_(std::move(mark)),
      inputCount_(inputCount),
      outputCount_(outputCount),
      inputFormats_(static_cast<size_t>(inputCount)),
      outputFormats_(static_cast<size_t>(outputCount)) {
    if (inputCount < 0 || outputCount < 0) {
        throw std::invalid_argument("FilterGraph layer pin count cannot be negative");
    }
}

void BaseLayer::setVisable(bool visible) {
    if (visible_ == visible) return;
    visible_ = visible;
    if (graph_) graph_->reset();
}

void BaseLayer::setEnable(bool enabled) {
    if (enabled_ == enabled) return;
    enabled_ = enabled;
    if (graph_) graph_->reset();
}

void BaseLayer::setStartNode(IBaseLayer* node, int32_t, int32_t) {
    startNode_ = node;
}

void BaseLayer::setEndNode(IBaseLayer* node) {
    endNode_ = node;
}

IBaseLayer* BaseLayer::addNode(IBaseLayer* layer) {
    return graph_ ? graph_->addNode(layer) : nullptr;
}

IBaseLayer* BaseLayer::addNode(ILayer* layer) {
    return graph_ ? graph_->addNode(layer) : nullptr;
}

IBaseLayer* BaseLayer::addLine(IBaseLayer* to, int32_t fromOut, int32_t toIn) {
    if (!graph_ || !to || !graph_->addLine(this, to, fromOut, toIn)) {
        return nullptr;
    }
    return to;
}

void BaseLayer::invalidateGraph() {
    if (graph_) graph_->reset();
}

void BaseLayer::setInputFormat(int32_t index, const ImageFormat& format) {
    if (index < 0 || index >= inputCount_) {
        throw std::out_of_range("FilterGraph input pin index is out of range");
    }
    inputFormats_[static_cast<size_t>(index)] = format;
}

void BaseLayer::setOutputFormat(int32_t index, const ImageFormat& format) {
    if (index < 0 || index >= outputCount_) {
        throw std::out_of_range("FilterGraph output pin index is out of range");
    }
    outputFormats_[static_cast<size_t>(index)] = format;
}

void BaseLayer::attach(PipeGraph* graph, int32_t graphIndex) {
    graph_ = graph;
    graphIndex_ = graphIndex;
}

void BaseLayer::detach() {
    graph_ = nullptr;
    graphIndex_ = -1;
}

} // namespace heisenberg::filtergraph
