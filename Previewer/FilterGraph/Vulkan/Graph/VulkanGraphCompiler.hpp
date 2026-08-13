#pragma once

#include "VulkanGraphDocument.hpp"
#include <unordered_map>

namespace heisenberg::filtergraph {

class IBaseNode;
class IInputNode;
class IOutputNode;
class IPipeGraph;
class VulkanNodeFactory;

struct VulkanCompiledGraph {
    IInputNode* input = nullptr;
    IOutputNode* output = nullptr;
    std::unordered_map<VulkanGraphNodeId, IBaseNode*> nodes;
};

class VulkanGraphCompiler {
public:
    static bool compile(const VulkanGraphDocument& document,
                        VulkanNodeFactory& nodeFactory,
                        IPipeGraph& graph,
                        VulkanCompiledGraph& result,
                        std::string* error = nullptr);
};

} // namespace heisenberg::filtergraph
