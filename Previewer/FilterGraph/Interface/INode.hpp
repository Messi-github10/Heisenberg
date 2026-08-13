#pragma once

#include "FilterCommon.hpp"

namespace heisenberg::filtergraph {

class IBaseNode;
class IPipeGraph;
class INode;

class IBaseNode {
public:
    virtual ~IBaseNode() = default;

    virtual const char* getMark() = 0;
    virtual bool bAttachGraph() = 0;
    virtual void setVisable(bool visible) = 0;
    virtual void setEnable(bool enabled) = 0;
    virtual int32_t getGraphIndex() = 0;

    virtual void setStartNode(IBaseNode* node, int32_t index = 0,
                              int32_t toInIndex = 0) = 0;
    virtual void setEndNode(IBaseNode* node) = 0;

    virtual IBaseNode* addNode(IBaseNode* node) = 0;
    virtual IBaseNode* addNode(INode* node) = 0;

    virtual IBaseNode* addLine(IBaseNode* to, int32_t fromOut = 0,
                               int32_t toIn = 0) = 0;

protected:
    template<typename T>
    friend class ITNode;
    virtual void onUpdateParamet() = 0;
};

class INode {
public:
    virtual ~INode() = default;
    virtual IBaseNode* getNode() = 0;
};

template<typename T>
class ITNode : public INode {
protected:
    T oldParamet = {};
    T paramet = {};

public:
    ITNode() = default;
    ~ITNode() override = default;

    void updateParamet(const T& value) {
        oldParamet = paramet;
        paramet = value;
        getNode()->onUpdateParamet();
    }

    T getParamet() const { return paramet; }
};

} // namespace heisenberg::filtergraph
