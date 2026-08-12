#pragma once

#include "VulkanGraphDocument.hpp"

#include <unordered_map>

namespace heisenberg::filtergraph {

class IBaseLayer;
class IInputLayer;
class IOutputLayer;
class IPipeGraph;
class VulkanNodeFactory;

struct VulkanCompiledGraph {
    IInputLayer* input = nullptr;
    IOutputLayer* output = nullptr;
    std::unordered_map<VulkanGraphNodeId, IBaseLayer*> nodes;
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
