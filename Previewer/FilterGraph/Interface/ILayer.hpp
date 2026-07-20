//
// Created by NiceFold on 2026/7/20.
//

#pragma once

#include "../Common/FilterCommon.hpp"

namespace heisenberg {
namespace filtergraph {

class IBaseLayer;
class IPipeGraph;
class ILayer;

// ============================================================
// IBaseLayer — 图中一个节点的抽象接口
// ============================================================

class IBaseLayer {
public:
    virtual ~IBaseLayer() = default;

    virtual const char* getMark()         = 0;
    virtual bool        bAttachGraph()    = 0;
    virtual void        setVisable(bool bVisable)   = 0;
    virtual void        setEnable(bool bEnable)     = 0;
    virtual int32_t     getGraphIndex()   = 0;

    /// 如果层有多个输入，不同输入可能对应不同层内不同层
    /// @param index    输入节点索引
    /// @param node     层内层节点
    /// @param toInIndex 对应层内层输入位置
    virtual void setStartNode(IBaseLayer* node, int32_t index = 0,
                              int32_t toInIndex = 0) = 0;
    virtual void setEndNode(IBaseLayer* node) = 0;

    virtual IBaseLayer* addNode(IBaseLayer* layer) = 0;
    virtual IBaseLayer* addNode(ILayer* layer)     = 0;

    virtual IBaseLayer* addLine(IBaseLayer* to, int32_t fromOut = 0,
                                int32_t toIn = 0) = 0;

protected:
    template<typename T>
    friend class ITLayer;
    virtual void onUpdateParamet() = 0;
};

// ============================================================
// ILayer — 用户实现层（不是抽象层）
// ============================================================

class ILayer {
public:
    virtual ~ILayer() = default;

    /// 请看上方 FG_LAYER_QUERYINTERFACE 宏提供的默认实现
    virtual IBaseLayer* getLayer() = 0;
};

// ============================================================
// ITLayer<T> — 带参数的层接口
// ============================================================

template<typename T>
class ITLayer : public ILayer {
protected:
    T oldParamet = {};
    T paramet    = {};

public:
    ITLayer()  = default;
    ~ITLayer() override = default;

    void updateParamet(const T& t) {
        oldParamet = this->paramet;
        this->paramet = t;
        getLayer()->onUpdateParamet();
    }

    T getParamet() const { return paramet; }
};

} // namespace filtergraph
} // namespace heisenberg
