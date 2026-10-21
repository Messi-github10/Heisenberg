#pragma once

#include "FilterCommon.hpp"
#include <MultiMedia/Video/Renderer/FilterGraph/Common/GpuProgram.hpp>
#include <memory>
#include <optional>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace heisenberg::filtergraph {

using VulkanGraphNodeId = uint64_t;

inline constexpr VulkanGraphNodeId kVulkanGraphInputNodeId = 1;
inline constexpr VulkanGraphNodeId kVulkanGraphOutputNodeId = 2;

using VulkanParameterValue = std::variant<int32_t, float, bool>;
using VulkanGraphParameter = std::unordered_map<std::string, VulkanParameterValue>;

struct VulkanGraphPosition {
    float x = 0.0f;
    float y = 0.0f;
};

struct VulkanGraphNodeDesc {
    VulkanGraphNodeId id = 0;
    std::string filterId;
    VulkanGraphParameter parameter = {};
    VulkanGraphPosition position = {};
    // Runtime-only program nodes are never loaded from persisted JSON.
    std::shared_ptr<const GpuProgram> program;
    std::optional<GraphImageContract> inputContract;
    std::optional<GraphImageContract> outputContract;
};

struct NodePin {
    uint64_t nodeId = 0;
    int32_t pinIndex = -1;

    constexpr bool operator==(const NodePin&) const = default;
};

struct GraphEdge {
    NodePin output;
    NodePin input;

    constexpr bool operator==(const GraphEdge&) const = default;
};

/// Editable graph data. Vulkan objects and shader paths never live here.
class VulkanGraphDocument {
public:
    VulkanGraphDocument() = default;
    // Empty stage has fixed endpoints but no edge; passthrough is runnable.
    static VulkanGraphDocument emptyStage(GraphImageContract contract = kWorkingImageContract);
    static VulkanGraphDocument passthrough(GraphImageContract contract = kWorkingImageContract);

    bool loadFromJsonFile(const std::string& path,
                          std::string* error = nullptr);

    uint32_t version() const {
        return version_;
    }
    const std::vector<VulkanGraphNodeDesc>& nodes() const {
        return nodes_;
    }
    const std::vector<GraphEdge>& edges() const {
        return edges_;
    }

    VulkanGraphNodeId addNode(
        std::string filterId,
        VulkanGraphParameter parameter = {},
        VulkanGraphPosition position = {},
        std::optional<GraphImageContract> inputContract = {},
        std::optional<GraphImageContract> outputContract = {});
    VulkanGraphNodeId addProgramNode(
        std::shared_ptr<const GpuProgram> program,
        GraphImageContract inputContract,
        GraphImageContract outputContract,
        VulkanGraphPosition position = {});
    bool removeNode(VulkanGraphNodeId nodeId);
    bool connect(NodePin output, NodePin input);
    bool disconnect(NodePin output, NodePin input);
    bool updateParameter(VulkanGraphNodeId nodeId,
                         VulkanGraphParameter parameter);
    bool moveNode(VulkanGraphNodeId nodeId, VulkanGraphPosition position);

    bool validate(std::string* error = nullptr) const;

private:
    VulkanGraphNodeDesc* findNode(VulkanGraphNodeId nodeId);
    const VulkanGraphNodeDesc* findNode(VulkanGraphNodeId nodeId) const;
    bool wouldCreateCycle(VulkanGraphNodeId fromNode,
                          VulkanGraphNodeId toNode) const;

    uint32_t version_ = 1;
    VulkanGraphNodeId nextNodeId_ = 3;
    std::vector<VulkanGraphNodeDesc> nodes_;
    std::vector<GraphEdge> edges_;
};

} // namespace heisenberg::filtergraph
