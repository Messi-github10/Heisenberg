#pragma once

#include <FilterGraph/Common/FilterCommon.hpp>

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
    GaussianBlurParamet>;

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

struct VulkanGraphEdgeDesc {
    VulkanGraphNodeId fromNode = 0;
    int32_t fromPin = 0;
    VulkanGraphNodeId toNode = 0;
    int32_t toPin = 0;
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
    const std::vector<VulkanGraphEdgeDesc>& edges() const { return edges_; }

    VulkanGraphNodeId addNode(
        VulkanGraphNodeType type,
        VulkanGraphParameter parameter = {},
        VulkanGraphPosition position = {});
    bool removeNode(VulkanGraphNodeId nodeId);
    bool connect(VulkanGraphNodeId fromNode, int32_t fromPin,
                 VulkanGraphNodeId toNode, int32_t toPin);
    bool disconnect(VulkanGraphNodeId fromNode, int32_t fromPin,
                    VulkanGraphNodeId toNode, int32_t toPin);
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
    std::vector<VulkanGraphEdgeDesc> edges_;
};

} // namespace heisenberg::filtergraph
