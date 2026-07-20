//
// Created by NiceFold on 2026/7/20.
//

#pragma once

#include "../Common/FilterCommon.hpp"
#include "ILayer.hpp"

namespace heisenberg {
namespace filtergraph {

// ============================================================
// IPipeGraph — DAG 滤镜图的抽象接口
// ============================================================

class IPipeGraph {
public:
    virtual ~IPipeGraph() = default;

    virtual GpuType getGpuType()   = 0;
    virtual void    reset()        = 0;

    virtual IBaseLayer* getNode(int32_t index) = 0;

    virtual IBaseLayer* addNode(IBaseLayer* layer) = 0;
    virtual IBaseLayer* addNode(ILayer* layer)     = 0;

    virtual bool addLine(int32_t from, int32_t to,
                         int32_t fromOut = 0, int32_t toIn = 0) = 0;
    virtual bool addLine(IBaseLayer* from, IBaseLayer* to,
                         int32_t fromOut = 0, int32_t toIn = 0) = 0;

    /// 查询某节点的输出格式（用于外部获取最终输出尺寸/类型）
    virtual bool getLayerOutFormat(int32_t  nodeIndex,
                                   int32_t  outputIndex,
                                   ImageFormat& format) = 0;

    /// 查询某节点的输入格式
    virtual bool getLayerInFormat(int32_t   nodeIndex,
                                  int32_t   inputIndex,
                                  ImageFormat& format) = 0;

    /// 清除连线（触发执行列表重组）
    virtual void clearLines() = 0;

    /// 清除全部节点 + 连线
    virtual void clear() = 0;

    /// 执行一帧
    virtual bool run() = 0;
};

// ============================================================
// PipeGraphFactory — 图工厂抽象
// ============================================================

class PipeGraphFactory {
public:
    virtual ~PipeGraphFactory() = default;
    virtual IPipeGraph* createGraph() = 0;
};

} // namespace filtergraph
} // namespace heisenberg
