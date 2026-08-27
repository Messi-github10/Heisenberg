#pragma once

#include "Graph/VulkanGraphDocument.hpp"
#include "Nodes/VulkanComputeNode.hpp"

#include <QJsonObject>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace heisenberg::filtergraph {

enum class VulkanFilterKind : uint8_t {
    compute,
    multiPass,
    stateful,
    readback,
    input,
    output,
};

enum class VulkanFilterValueType : uint8_t {
    integer,
    real,
    boolean,
};

struct VulkanFilterParameterDesc {
    std::string name;
    VulkanFilterValueType type = VulkanFilterValueType::real;
    size_t offset = 0;
    double defaultValue = 0.0;
    double minimum = 0.0;
    double maximum = 0.0;
    bool hasMinimum = false;
    bool hasMaximum = false;
    bool exposed = true;
};

struct VulkanFilterPassDescriptor {
    std::string name;
    int32_t directionX = 0;
    int32_t directionY = 0;
};

struct VulkanFilterExtraInputDescriptor {
    std::string name;
    VulkanInputBinding binding = VulkanInputBinding::sampledLinear;
};

struct VulkanFilterDescriptor {
    std::string id;
    std::string displayName;
    VulkanFilterKind kind = VulkanFilterKind::compute;
    std::string shaderSource;
    std::string shaderBinary;
    int32_t inputCount = 1;
    int32_t outputCount = 1;
    std::vector<VulkanInputBinding> inputBindings;
    std::vector<VulkanFilterExtraInputDescriptor> extraInputs;
    std::string auxiliarySource;
    uint32_t auxiliaryWidth = 0;
    uint32_t auxiliaryHeight = 0;
    size_t uniformSize = 0;
    std::vector<VulkanFilterParameterDesc> parameters;
    bool resizeOutput = false;
    bool passthroughOutput = false;
    size_t readbackSize = 0;
    uint32_t readbackBinding = 0;
    bool clearReadbackBuffer = false;
    std::vector<VulkanFilterPassDescriptor> passes;
};

class VulkanFilterRegistry final {
public:
    static VulkanFilterRegistry& instance();

    bool ensureLoaded(std::string* error = nullptr);
    const VulkanFilterDescriptor* find(std::string_view id,
                                       std::string* error = nullptr);
    const VulkanFilterDescriptor* find(VulkanGraphNodeType type,
                                       std::string* error = nullptr);

    bool parseParameters(const VulkanFilterDescriptor& descriptor,
                         const QJsonObject& object,
                         VulkanGraphParameter& result,
                         std::string* error = nullptr) const;
    bool validateParameters(const VulkanFilterDescriptor& descriptor,
                            const VulkanGraphParameter& parameter,
                            std::string* error = nullptr) const;

    const std::vector<VulkanFilterDescriptor>& descriptors() const {
        return descriptors_;
    }

private:
    bool load(std::string* error);

    bool loaded_ = false;
    std::vector<VulkanFilterDescriptor> descriptors_;
};

const char* vulkanGraphNodeTypeName(VulkanGraphNodeType type) noexcept;

} // namespace heisenberg::filtergraph
