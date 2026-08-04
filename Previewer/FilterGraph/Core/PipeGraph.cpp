#include "PipeGraph.hpp"

#include "BaseLayer.hpp"

#include <Utiles/Logger.hpp>

#include <algorithm>
#include <queue>

namespace heisenberg::filtergraph {

PipeGraph::PipeGraph(GpuType gpuType) : gpuType_(gpuType) {}

PipeGraph::~PipeGraph() {
    clear();
}

void PipeGraph::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    dirty_ = true;
}

IBaseLayer* PipeGraph::getNode(int32_t index) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (index < 0 || index >= static_cast<int32_t>(nodes_.size())) return nullptr;
    return nodes_[static_cast<size_t>(index)];
}

IBaseLayer* PipeGraph::addNode(IBaseLayer* layer) {
    auto* base = dynamic_cast<BaseLayer*>(layer);
    if (!base) {
        LOG_ERROR("FilterGraph: node is not a BaseLayer");
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (base->bAttachGraph()) {
        LOG_ERROR("FilterGraph: layer '{}' is already attached", base->getMark());
        return nullptr;
    }
    base->attach(this, static_cast<int32_t>(nodes_.size()));
    nodes_.push_back(base);
    dirty_ = true;
    return base;
}

IBaseLayer* PipeGraph::addNode(ILayer* layer) {
    return layer ? addNode(layer->getLayer()) : nullptr;
}

bool PipeGraph::validateEdge(const GraphEdge& edge) const {
    if (edge.fromNode < 0 || edge.toNode < 0 || edge.fromNode == edge.toNode
        || edge.fromNode >= static_cast<int32_t>(nodes_.size())
        || edge.toNode >= static_cast<int32_t>(nodes_.size())) {
        return false;
    }
    return edge.fromPin >= 0
        && edge.fromPin < nodes_[static_cast<size_t>(edge.fromNode)]->outputCount()
        && edge.toPin >= 0
        && edge.toPin < nodes_[static_cast<size_t>(edge.toNode)]->inputCount();
}

bool PipeGraph::addLine(int32_t from, int32_t to, int32_t fromOut,
                        int32_t toIn) {
    std::lock_guard<std::mutex> lock(mutex_);
    GraphEdge edge{from, fromOut, to, toIn};
    if (!validateEdge(edge)) {
        LOG_ERROR("FilterGraph: invalid edge {}:{} -> {}:{}",
                  from, fromOut, to, toIn);
        return false;
    }

    const auto duplicate = std::find_if(edges_.begin(), edges_.end(),
        [&](const GraphEdge& item) {
            return item.fromNode == from && item.fromPin == fromOut
                && item.toNode == to && item.toPin == toIn;
        });
    if (duplicate != edges_.end()) return true;

    const auto occupied = std::find_if(edges_.begin(), edges_.end(),
        [&](const GraphEdge& item) {
            return item.toNode == to && item.toPin == toIn;
        });
    if (occupied != edges_.end()) {
        LOG_ERROR("FilterGraph: input pin {}:{} already has a connection", to, toIn);
        return false;
    }

    edges_.push_back(edge);
    dirty_ = true;
    return true;
}

bool PipeGraph::addLine(IBaseLayer* from, IBaseLayer* to, int32_t fromOut,
                        int32_t toIn) {
    if (!from || !to) return false;
    return addLine(from->getGraphIndex(), to->getGraphIndex(), fromOut, toIn);
}

bool PipeGraph::getLayerOutFormat(int32_t nodeIndex, int32_t outputIndex,
                                  ImageFormat& format) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (nodeIndex < 0 || nodeIndex >= static_cast<int32_t>(nodes_.size())) return false;
    const auto& formats = nodes_[static_cast<size_t>(nodeIndex)]->outputFormats();
    if (outputIndex < 0 || outputIndex >= static_cast<int32_t>(formats.size())) return false;
    format = formats[static_cast<size_t>(outputIndex)];
    return true;
}

bool PipeGraph::getLayerInFormat(int32_t nodeIndex, int32_t inputIndex,
                                 ImageFormat& format) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (nodeIndex < 0 || nodeIndex >= static_cast<int32_t>(nodes_.size())) return false;
    const auto& formats = nodes_[static_cast<size_t>(nodeIndex)]->inputFormats();
    if (inputIndex < 0 || inputIndex >= static_cast<int32_t>(formats.size())) return false;
    format = formats[static_cast<size_t>(inputIndex)];
    return true;
}

void PipeGraph::clearLines() {
    std::lock_guard<std::mutex> lock(mutex_);
    edges_.clear();
    executionOrder_.clear();
    dirty_ = true;
}

void PipeGraph::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    onGraphCleared();
    edges_.clear();
    executionOrder_.clear();
    for (BaseLayer* node : nodes_) {
        if (node) node->detach();
    }
    nodes_.clear();
    dirty_ = true;
}

const GraphEdge* PipeGraph::inputEdge(int32_t nodeIndex, int32_t inputPin) const {
    const auto found = std::find_if(edges_.begin(), edges_.end(),
        [&](const GraphEdge& edge) {
            return edge.toNode == nodeIndex && edge.toPin == inputPin;
        });
    return found == edges_.end() ? nullptr : &*found;
}

bool PipeGraph::rebuild() {
    std::vector<int32_t> indegree(nodes_.size(), 0);
    std::vector<std::vector<int32_t>> outgoing(nodes_.size());

    for (const GraphEdge& edge : edges_) {
        if (!validateEdge(edge)) return false;
        ++indegree[static_cast<size_t>(edge.toNode)];
        outgoing[static_cast<size_t>(edge.fromNode)].push_back(edge.toNode);
    }

    std::queue<int32_t> ready;
    for (int32_t i = 0; i < static_cast<int32_t>(nodes_.size()); ++i) {
        if (indegree[static_cast<size_t>(i)] == 0) ready.push(i);
    }

    executionOrder_.clear();
    while (!ready.empty()) {
        const int32_t node = ready.front();
        ready.pop();
        executionOrder_.push_back(node);
        for (int32_t next : outgoing[static_cast<size_t>(node)]) {
            if (--indegree[static_cast<size_t>(next)] == 0) ready.push(next);
        }
    }

    if (executionOrder_.size() != nodes_.size()) {
        LOG_ERROR("FilterGraph: graph contains a cycle");
        executionOrder_.clear();
        return false;
    }

    for (int32_t nodeIndex : executionOrder_) {
        BaseLayer* node = nodes_[static_cast<size_t>(nodeIndex)];
        std::vector<ImageFormat> inputs(static_cast<size_t>(node->inputCount()));
        for (int32_t pin = 0; pin < node->inputCount(); ++pin) {
            const GraphEdge* edge = inputEdge(nodeIndex, pin);
            if (!edge) {
                LOG_ERROR("FilterGraph: layer '{}' input pin {} is not connected",
                          node->getMark(), pin);
                return false;
            }
            const auto& sourceFormats =
                nodes_[static_cast<size_t>(edge->fromNode)]->outputFormats();
            inputs[static_cast<size_t>(pin)] =
                sourceFormats[static_cast<size_t>(edge->fromPin)];
        }
        if (!node->configure(inputs)) {
            LOG_ERROR("FilterGraph: failed to configure layer '{}'", node->getMark());
            return false;
        }
    }

    if (!onGraphRebuilt()) return false;
    dirty_ = false;
    return true;
}

bool PipeGraph::run(const FrameContext& frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (dirty_ && !rebuild()) return false;
    return onRun(frame);
}

} // namespace heisenberg::filtergraph
