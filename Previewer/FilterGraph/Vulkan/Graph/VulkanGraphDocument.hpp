#pragma once

#include <FilterGraph/Common/FilterCommon.hpp>
#include <FilterGraph/Vulkan/Filters/GaussianBlurParams.hpp>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace heisenberg::filtergraph {

using VulkanGraphNodeId = uint64_t;

inline constexpr VulkanGraphNodeId kVulkanGraphInputNodeId = 1;
inline constexpr VulkanGraphNodeId kVulkanGraphOutputNodeId = 2;

enum class VulkanGraphNodeType : uint8_t {
    input,
    output,
    colorInvert,
    exposure,
    blend,
    gaussianBlur,
};

struct ExposureParamet {
    float exposure = 0.0f;
};

struct BlendParamet {
    float factor = 0.5f;
};

using VulkanGraphParameter = std::variant<
    std::monostate,
    ExposureParamet,
    BlendParamet,
    GaussianBlurParams>;

struct VulkanGraphPosition {
    float x = 0.0f;
    float y = 0.0f;
};

struct VulkanGraphNodeDesc {
    VulkanGraphNodeId id = 0;
    VulkanGraphNodeType type = VulkanGraphNodeType::colorInvert;
    VulkanGraphParameter parameter = {};
    VulkanGraphPosition position = {};
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

    static const char* testGraphPath();
    bool loadFromJsonFile(const std::string& path,
                          std::string* error = nullptr);

    uint32_t version() const { return version_; }
    const std::vector<VulkanGraphNodeDesc>& nodes() const { return nodes_; }
    const std::vector<GraphEdge>& edges() const { return edges_; }

    VulkanGraphNodeId addNode(
        VulkanGraphNodeType type,
        VulkanGraphParameter parameter = {},
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
