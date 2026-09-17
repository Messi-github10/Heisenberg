#pragma once

#include "FilterCommon.hpp"

namespace heisenberg::filtergraph {

class IBaseNode;
class IPipeGraph;
class IFilterNode;

class IBaseNode {
public:
    virtual ~IBaseNode() = default;

    virtual const char* getMark() = 0;
    virtual bool bAttachGraph() = 0;
    virtual void setVisable(bool visible) = 0;
    virtual void setEnable(bool enabled) = 0;
    virtual int32_t getGraphIndex() = 0;

    virtual IBaseNode* addNode(IBaseNode* node) = 0;
    virtual IBaseNode* addNode(IFilterNode* node) = 0;

    virtual IBaseNode* addLine(IBaseNode* to, int32_t fromOut = 0,
                               int32_t toIn = 0) = 0;
};

class IFilterNode {
public:
    virtual ~IFilterNode() = default;
    virtual IBaseNode* getNode() = 0;
};

} // namespace heisenberg::filtergraph
