#pragma once

#include <FilterGraph/Interface/ILayer.hpp>

#include <string>
#include <vector>

namespace heisenberg::filtergraph {

class PipeGraph;

class BaseLayer : public virtual ILayer, public IBaseLayer {
public:
    BaseLayer(std::string mark, int32_t inputCount, int32_t outputCount);
    ~BaseLayer() override = default;

    IBaseLayer* getLayer() override { return this; }
    const char* getMark() override { return mark_.c_str(); }
    bool bAttachGraph() override { return graph_ != nullptr; }
    void setVisable(bool visible) override;
    void setEnable(bool enabled) override;
    int32_t getGraphIndex() override { return graphIndex_; }

    void setStartNode(IBaseLayer* node, int32_t index = 0,
                      int32_t toInIndex = 0) override;
    void setEndNode(IBaseLayer* node) override;
    IBaseLayer* addNode(IBaseLayer* layer) override;
    IBaseLayer* addNode(ILayer* layer) override;
    IBaseLayer* addLine(IBaseLayer* to, int32_t fromOut = 0,
                        int32_t toIn = 0) override;

    int32_t inputCount() const { return inputCount_; }
    int32_t outputCount() const { return outputCount_; }
    bool enabled() const { return enabled_; }
    bool visible() const { return visible_; }
    bool active() const { return enabled_ && visible_; }

    const std::vector<ImageFormat>& inputFormats() const { return inputFormats_; }
    const std::vector<ImageFormat>& outputFormats() const { return outputFormats_; }

protected:
    void onUpdateParamet() override {}

    void invalidateGraph();
    void setInputFormat(int32_t index, const ImageFormat& format);
    void setOutputFormat(int32_t index, const ImageFormat& format);
    virtual bool configure(const std::vector<ImageFormat>& inputs) = 0;

private:
    friend class PipeGraph;

    void attach(PipeGraph* graph, int32_t graphIndex);
    void detach();

    std::string mark_;
    int32_t inputCount_  = 0;
    int32_t outputCount_ = 0;
    int32_t graphIndex_  = -1;
    bool enabled_        = true;
    bool visible_        = true;
    PipeGraph* graph_    = nullptr;
    IBaseLayer* startNode_ = nullptr;
    IBaseLayer* endNode_   = nullptr;
    std::vector<ImageFormat> inputFormats_;
    std::vector<ImageFormat> outputFormats_;
};

} // namespace heisenberg::filtergraph
