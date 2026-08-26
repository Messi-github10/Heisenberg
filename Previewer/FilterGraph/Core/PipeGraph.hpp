#pragma once

#include <FilterGraph/Interface/IPipeGraph.hpp>
#include <mutex>
#include <vector>

namespace heisenberg::filtergraph {

class BaseNode;

struct RuntimeGraphEdge {
    int32_t fromNode = -1;
    int32_t fromPin  = -1;
    int32_t toNode   = -1;
    int32_t toPin    = -1;
};

class PipeGraph : public IPipeGraph {
public:
    explicit PipeGraph(GraphicApiBackend gpuType);
    ~PipeGraph() override;

    GraphicApiBackend getGpuType() override { return gpuType_; }
    void reset() override;
    IBaseNode* getNode(int32_t index) override;
    IBaseNode* addNode(IBaseNode* node) override;
    IBaseNode* addNode(INode* node) override;
    bool addLine(int32_t from, int32_t to, int32_t fromOut = 0,
                 int32_t toIn = 0) override;
    bool addLine(IBaseNode* from, IBaseNode* to, int32_t fromOut = 0,
                 int32_t toIn = 0) override;
    bool getNodeOutFormat(int32_t nodeIndex, int32_t outputIndex,
                           ImageFormat& format) override;
    bool getNodeInFormat(int32_t nodeIndex, int32_t inputIndex,
                          ImageFormat& format) override;
    void clearLines() override;
    void clear() override;
    bool run(const FrameContext& frame) override;

protected:
    const std::vector<BaseNode*>& nodes() const { return nodes_; }
    const std::vector<RuntimeGraphEdge>& edges() const { return edges_; }
    const std::vector<int32_t>& executionOrder() const { return executionOrder_; }

    const RuntimeGraphEdge* inputEdge(int32_t nodeIndex,
                                      int32_t inputPin) const;
    virtual bool onGraphRebuilt() = 0;
    virtual bool onRun(const FrameContext& frame) = 0;
    virtual void onGraphCleared() {}

private:
    bool rebuild();
    bool validateEdge(const RuntimeGraphEdge& edge) const;

    GraphicApiBackend gpuType_ = GraphicApiBackend::other;
    std::vector<BaseNode*> nodes_;
    std::vector<RuntimeGraphEdge> edges_;
    std::vector<int32_t> executionOrder_;
    bool dirty_ = true;
    mutable std::mutex mutex_;
};

} // namespace heisenberg::filtergraph
