#pragma once

#include <FilterGraph/Interface/IPipeGraph.hpp>

#include <mutex>
#include <vector>

namespace heisenberg::filtergraph {

class BaseLayer;

struct GraphEdge {
    int32_t fromNode = -1;
    int32_t fromPin  = -1;
    int32_t toNode   = -1;
    int32_t toPin    = -1;
};

class PipeGraph : public IPipeGraph {
public:
    explicit PipeGraph(GpuType gpuType);
    ~PipeGraph() override;

    GpuType getGpuType() override { return gpuType_; }
    void reset() override;
    IBaseLayer* getNode(int32_t index) override;
    IBaseLayer* addNode(IBaseLayer* layer) override;
    IBaseLayer* addNode(ILayer* layer) override;
    bool addLine(int32_t from, int32_t to, int32_t fromOut = 0,
                 int32_t toIn = 0) override;
    bool addLine(IBaseLayer* from, IBaseLayer* to, int32_t fromOut = 0,
                 int32_t toIn = 0) override;
    bool getLayerOutFormat(int32_t nodeIndex, int32_t outputIndex,
                           ImageFormat& format) override;
    bool getLayerInFormat(int32_t nodeIndex, int32_t inputIndex,
                          ImageFormat& format) override;
    void clearLines() override;
    void clear() override;
    bool run(const FrameContext& frame) override;

protected:
    const std::vector<BaseLayer*>& nodes() const { return nodes_; }
    const std::vector<GraphEdge>& edges() const { return edges_; }
    const std::vector<int32_t>& executionOrder() const { return executionOrder_; }

    const GraphEdge* inputEdge(int32_t nodeIndex, int32_t inputPin) const;
    virtual bool onGraphRebuilt() = 0;
    virtual bool onRun(const FrameContext& frame) = 0;
    virtual void onGraphCleared() {}

private:
    bool rebuild();
    bool validateEdge(const GraphEdge& edge) const;

    GpuType gpuType_ = GpuType::other;
    std::vector<BaseLayer*> nodes_;
    std::vector<GraphEdge> edges_;
    std::vector<int32_t> executionOrder_;
    bool dirty_ = true;
    mutable std::mutex mutex_;
};

} // namespace heisenberg::filtergraph
