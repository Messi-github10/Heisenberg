//
// Created by NiceFold on 2026/7/20.
//

#pragma once

#include "FilterCommon.hpp"
#include "INode.hpp"

namespace heisenberg {
namespace filtergraph {

class IPipeGraph {
public:
    virtual ~IPipeGraph() = default;

    virtual GraphicApiBackend getGpuType()   = 0;
    virtual void    reset()        = 0;

    virtual IBaseNode* getNode(int32_t index) = 0;

    virtual IBaseNode* addNode(IBaseNode* node) = 0;
    virtual IBaseNode* addNode(INode* node) = 0;

    virtual bool addLine(int32_t from, int32_t to,
                         int32_t fromOut = 0, int32_t toIn = 0) = 0;
    virtual bool addLine(IBaseNode* from, IBaseNode* to,
                         int32_t fromOut = 0, int32_t toIn = 0) = 0;

    virtual bool getNodeOutFormat(int32_t  nodeIndex,
                                   int32_t  outputIndex,
                                   ImageFormat& format) = 0;

    virtual bool getNodeInFormat(int32_t   nodeIndex,
                                  int32_t   inputIndex,
                                  ImageFormat& format) = 0;

    virtual void clearLines() = 0;

    virtual void clear() = 0;

    virtual bool run(const FrameContext& frame) = 0;
};

class PipeGraphFactory {
public:
    virtual ~PipeGraphFactory() = default;
    virtual IPipeGraph* createGraph(const VulkanGraphContext& context) = 0;
};

} // namespace filtergraph
} // namespace heisenberg
