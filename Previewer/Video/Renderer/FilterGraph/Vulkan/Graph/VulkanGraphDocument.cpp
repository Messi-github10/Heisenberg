#include "VulkanGraphDocument.hpp"
#include "../VulkanFilterRegistry.hpp"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QString>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace heisenberg::filtergraph {
namespace {

struct PinCounts {
    int32_t inputs = 0;
    int32_t outputs = 0;
};

const VulkanFilterDescriptor* descriptorFor(
    const VulkanGraphNodeDesc& node, std::string* error = nullptr) {
    return VulkanFilterRegistry::instance().find(node.filterId, error);
}

PinCounts pinCounts(const VulkanGraphNodeDesc& node) {
    if (node.filterId == "input") return {0, 1};
    if (node.filterId == "output") return {1, 0};
    const auto* descriptor = descriptorFor(node);
    return descriptor ? PinCounts{descriptor->inputCount, descriptor->outputCount}
                      : PinCounts{};
}

bool parameterMatches(const VulkanGraphNodeDesc& node) {
    if (node.filterId == "input" || node.filterId == "output") {
        return std::holds_alternative<std::monostate>(node.parameter);
    }
    std::string error;
    const auto* descriptor = descriptorFor(node, &error);
    if (!descriptor) return false;
    return VulkanFilterRegistry::instance().validateParameters(
        *descriptor, node.parameter, &error);
}

struct NodePinHash {
    size_t operator()(const NodePin& pin) const {
        const size_t nodeHash = std::hash<uint64_t>{}(pin.nodeId);
        const size_t pinHash = std::hash<int32_t>{}(pin.pinIndex);
        return nodeHash ^ (pinHash + 0x9e3779b9u + (nodeHash << 6)
            + (nodeHash >> 2));
    }
};

void setError(std::string* error, const char* message) {
    if (error) *error = message;
}

void setError(std::string* error, const QString& message) {
    if (error) *error = message.toStdString();
}

bool readNodeId(const QJsonObject& object, const char* key,
                VulkanGraphNodeId& result, std::string* error) {
    const QJsonValue value = object.value(QLatin1String(key));
    const double number = value.toDouble(-1.0);
    if (!value.isDouble() || number < 1.0
        || number > 9007199254740991.0
        || std::floor(number) != number) {
        setError(error, QStringLiteral("JSON field '%1' must be a positive integer")
            .arg(QLatin1String(key)));
        return false;
    }
    result = static_cast<VulkanGraphNodeId>(number);
    return true;
}

bool readPin(const QJsonObject& object, const char* key,
             int32_t& result, std::string* error) {
    const QJsonValue value = object.value(QLatin1String(key));
    const double number = value.toDouble(-1.0);
    if (!value.isDouble() || number < 0.0
        || number > static_cast<double>(std::numeric_limits<int32_t>::max())
        || std::floor(number) != number) {
        setError(error, QStringLiteral("JSON field '%1' must be a nonnegative integer")
            .arg(QLatin1String(key)));
        return false;
    }
    result = static_cast<int32_t>(number);
    return true;
}

bool readNodePin(const QJsonObject& edgeObject, const char* key,
                 NodePin& result, std::string* error) {
    const QJsonValue value = edgeObject.value(QLatin1String(key));
    if (!value.isObject()) {
        setError(error, QStringLiteral("JSON field '%1' must be an object")
            .arg(QLatin1String(key)));
        return false;
    }
    const QJsonObject object = value.toObject();
    return readNodeId(object, "nodeId", result.nodeId, error)
        && readPin(object, "pinIndex", result.pinIndex, error);
}

bool parseNodeType(const QString& name, VulkanGraphNodeType& type) {
    constexpr VulkanGraphNodeType types[] = {
        VulkanGraphNodeType::input, VulkanGraphNodeType::output,
        VulkanGraphNodeType::colorInvert, VulkanGraphNodeType::exposure,
        VulkanGraphNodeType::blend, VulkanGraphNodeType::gaussianBlur,
        VulkanGraphNodeType::resize, VulkanGraphNodeType::lut,
        VulkanGraphNodeType::histogram,
    };
    for (const VulkanGraphNodeType candidate : types) {
        if (name == QLatin1String(vulkanGraphNodeTypeName(candidate))) {
            type = candidate;
            return true;
        }
    }
    return false;
}

bool parseFilterId(const QString& name, std::string& filterId,
                   VulkanGraphNodeType& type, std::string* error) {
    if (name.isEmpty()) {
        setError(error, "Vulkan graph node has an empty filter id");
        return false;
    }
    filterId = name.toStdString();
    if (parseNodeType(name, type)) return true;
    std::string lookupError;
    if (!VulkanFilterRegistry::instance().find(filterId, &lookupError)) {
        setError(error, QStringLiteral("Vulkan graph node has an unregistered filter id '%1'")
            .arg(name));
        return false;
    }
    type = VulkanGraphNodeType::colorInvert;
    return true;
}

bool readFiniteFloat(const QJsonObject& object, const char* key,
                     float defaultValue, float& result,
                     std::string* error) {
    const QJsonValue value = object.value(QLatin1String(key));
    if (value.isUndefined()) {
        result = defaultValue;
        return true;
    }
    const double number = value.toDouble(
        std::numeric_limits<double>::quiet_NaN());
    if (!value.isDouble() || !std::isfinite(number)
        || number < -static_cast<double>(std::numeric_limits<float>::max())
        || number > static_cast<double>(std::numeric_limits<float>::max())) {
        setError(error, QStringLiteral("JSON field '%1' must be a finite number")
            .arg(QLatin1String(key)));
        return false;
    }
    result = static_cast<float>(number);
    return true;
}

bool parseParameter(const std::string& filterId, const QJsonObject& object,
                    VulkanGraphParameter& result, std::string* error) {
    if (filterId == "input" || filterId == "output") {
        result = std::monostate{};
        return object.isEmpty();
    }
    std::string lookupError;
    const auto* descriptor = VulkanFilterRegistry::instance().find(filterId, &lookupError);
    return descriptor && VulkanFilterRegistry::instance().parseParameters(
        *descriptor, object, result, error);
}

} // namespace

bool VulkanGraphDocument::loadFromJsonFile(
    const std::string& path, std::string* error) {
    QFile file(QString::fromStdString(path));
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, QStringLiteral("Failed to open Vulkan graph JSON '%1': %2")
            .arg(file.fileName(), file.errorString()));
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument json = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !json.isObject()) {
        setError(error, QStringLiteral("Invalid Vulkan graph JSON: %1")
            .arg(parseError.errorString()));
        return false;
    }

    VulkanGraphDocument parsed;
    const QJsonObject root = json.object();
    const int version = root.value("version").toInt(-1);
    if (version != 1) {
        setError(error, "Unsupported Vulkan graph JSON version");
        return false;
    }
    parsed.version_ = static_cast<uint32_t>(version);

    const QJsonValue nodesValue = root.value("nodes");
    const QJsonValue edgesValue = root.value("edges");
    if (!nodesValue.isArray() || !edgesValue.isArray()) {
        setError(error, "Vulkan graph JSON requires nodes and edges arrays");
        return false;
    }

    VulkanGraphNodeId maximumNodeId = 2;
    for (const QJsonValue& nodeValue : nodesValue.toArray()) {
        if (!nodeValue.isObject()) {
            setError(error, "Every Vulkan graph node must be an object");
            return false;
        }
        const QJsonObject nodeObject = nodeValue.toObject();
        VulkanGraphNodeDesc node;
        if (!readNodeId(nodeObject, "id", node.id, error)) return false;
        const QJsonValue filterValue = nodeObject.contains("filterId")
            ? nodeObject.value("filterId") : nodeObject.value("type");
        if (!filterValue.isString()
            || !parseFilterId(filterValue.toString(), node.filterId,
                              node.type, error)) {
            return false;
        }
        const QJsonValue parametersValue = nodeObject.value("parameters");
        if (!parametersValue.isUndefined() && !parametersValue.isObject()) {
            setError(error, "Vulkan graph node parameters must be an object");
            return false;
        }
        if (!parseParameter(node.filterId, parametersValue.toObject(),
                            node.parameter, error)) {
            return false;
        }
        const QJsonValue positionValue = nodeObject.value("position");
        if (!positionValue.isUndefined() && !positionValue.isObject()) {
            setError(error, "Vulkan graph node position must be an object");
            return false;
        }
        const QJsonObject position = positionValue.toObject();
        if (!readFiniteFloat(position, "x", 0.0f, node.position.x, error)
            || !readFiniteFloat(position, "y", 0.0f, node.position.y, error)) {
            return false;
        }
        parsed.nodes_.push_back(std::move(node));
        maximumNodeId = std::max(maximumNodeId, parsed.nodes_.back().id);
    }

    for (const QJsonValue& edgeValue : edgesValue.toArray()) {
        if (!edgeValue.isObject()) {
            setError(error, "Every Vulkan graph edge must be an object");
            return false;
        }
        const QJsonObject edgeObject = edgeValue.toObject();
        GraphEdge edge;
        if (!readNodePin(edgeObject, "output", edge.output, error)
            || !readNodePin(edgeObject, "input", edge.input, error)) {
            return false;
        }
        parsed.edges_.push_back(edge);
    }

    if (maximumNodeId == std::numeric_limits<VulkanGraphNodeId>::max()) {
        setError(error, "Vulkan graph node ID space is exhausted");
        return false;
    }
    parsed.nextNodeId_ = maximumNodeId + 1;
    if (!parsed.validate(error)) return false;
    *this = std::move(parsed);
    return true;
}

VulkanGraphNodeDesc* VulkanGraphDocument::findNode(
    VulkanGraphNodeId nodeId) {
    const auto found = std::find_if(nodes_.begin(), nodes_.end(),
        [nodeId](const VulkanGraphNodeDesc& node) { return node.id == nodeId; });
    return found == nodes_.end() ? nullptr : &*found;
}

const VulkanGraphNodeDesc* VulkanGraphDocument::findNode(
    VulkanGraphNodeId nodeId) const {
    const auto found = std::find_if(nodes_.begin(), nodes_.end(),
        [nodeId](const VulkanGraphNodeDesc& node) { return node.id == nodeId; });
    return found == nodes_.end() ? nullptr : &*found;
}

VulkanGraphNodeId VulkanGraphDocument::addNode(
    VulkanGraphNodeType type, VulkanGraphParameter parameter,
    VulkanGraphPosition position) {
    return addNode(std::string(vulkanGraphNodeTypeName(type)),
                   std::move(parameter), position);
}

VulkanGraphNodeId VulkanGraphDocument::addNode(
    std::string filterId, VulkanGraphParameter parameter,
    VulkanGraphPosition position) {
    if (filterId == "input" || filterId == "output") return 0;

    std::string error;
    const VulkanFilterDescriptor* descriptor =
        VulkanFilterRegistry::instance().find(filterId, &error);
    if (!descriptor) return 0;

    VulkanGraphNodeType legacyType = VulkanGraphNodeType::colorInvert;
    for (int value = 0; value <= static_cast<int>(VulkanGraphNodeType::histogram); ++value) {
        const auto candidate = static_cast<VulkanGraphNodeType>(value);
        if (filterId == vulkanGraphNodeTypeName(candidate)) {
            legacyType = candidate;
            break;
        }
    }

    if (std::holds_alternative<std::monostate>(parameter)) {
        VulkanGraphParameter defaults;
        if (!VulkanFilterRegistry::instance().parseParameters(
                *descriptor, QJsonObject{}, defaults, &error)) {
            return 0;
        }
        parameter = std::move(defaults);
    }

    VulkanGraphNodeDesc node;
    node.id = nextNodeId_;
    node.type = legacyType;
    node.filterId = std::move(filterId);
    node.parameter = std::move(parameter);
    node.position = position;
    if (!parameterMatches(node)) return 0;
    nodes_.push_back(std::move(node));
    return nextNodeId_++;
}

bool VulkanGraphDocument::removeNode(VulkanGraphNodeId nodeId) {
    if (nodeId == kVulkanGraphInputNodeId
        || nodeId == kVulkanGraphOutputNodeId) {
        return false;
    }
    const auto found = std::find_if(nodes_.begin(), nodes_.end(),
        [nodeId](const VulkanGraphNodeDesc& node) { return node.id == nodeId; });
    if (found == nodes_.end()) return false;
    nodes_.erase(found);
    std::erase_if(edges_, [nodeId](const GraphEdge& edge) {
        return edge.output.nodeId == nodeId || edge.input.nodeId == nodeId;
    });
    return true;
}

bool VulkanGraphDocument::wouldCreateCycle(
    VulkanGraphNodeId fromNode, VulkanGraphNodeId toNode) const {
    std::vector<VulkanGraphNodeId> pending{toNode};
    std::unordered_set<VulkanGraphNodeId> visited;
    while (!pending.empty()) {
        const VulkanGraphNodeId current = pending.back();
        pending.pop_back();
        if (current == fromNode) return true;
        if (!visited.insert(current).second) continue;
        for (const GraphEdge& edge : edges_) {
            if (edge.output.nodeId == current) {
                pending.push_back(edge.input.nodeId);
            }
        }
    }
    return false;
}

bool VulkanGraphDocument::connect(NodePin output, NodePin input) {
    const VulkanGraphNodeDesc* source = findNode(output.nodeId);
    const VulkanGraphNodeDesc* destination = findNode(input.nodeId);
    if (!source || !destination || output.nodeId == input.nodeId) return false;
    const PinCounts sourcePins = pinCounts(*source);
    const PinCounts destinationPins = pinCounts(*destination);
    if (output.pinIndex < 0 || output.pinIndex >= sourcePins.outputs
        || input.pinIndex < 0 || input.pinIndex >= destinationPins.inputs) {
        return false;
    }
    const auto occupied = std::find_if(edges_.begin(), edges_.end(),
        [input](const GraphEdge& edge) {
            return edge.input == input;
        });
    if (occupied != edges_.end()
        || wouldCreateCycle(output.nodeId, input.nodeId)) {
        return false;
    }
    edges_.push_back({output, input});
    return true;
}

bool VulkanGraphDocument::disconnect(NodePin output, NodePin input) {
    const auto found = std::find_if(edges_.begin(), edges_.end(),
        [output, input](const GraphEdge& edge) {
            return edge.output == output && edge.input == input;
        });
    if (found == edges_.end()) return false;
    edges_.erase(found);
    return true;
}

bool VulkanGraphDocument::updateParameter(
    VulkanGraphNodeId nodeId, VulkanGraphParameter parameter) {
    VulkanGraphNodeDesc* node = findNode(nodeId);
    if (!node) return false;
    VulkanGraphNodeDesc candidate = *node;
    candidate.parameter = std::move(parameter);
    if (!parameterMatches(candidate)) return false;
    node->parameter = std::move(candidate.parameter);
    return true;
}

bool VulkanGraphDocument::moveNode(
    VulkanGraphNodeId nodeId, VulkanGraphPosition position) {
    VulkanGraphNodeDesc* node = findNode(nodeId);
    if (!node) return false;
    node->position = position;
    return true;
}

bool VulkanGraphDocument::validate(std::string* error) const {
    std::unordered_map<VulkanGraphNodeId, const VulkanGraphNodeDesc*> nodesById;
    for (const VulkanGraphNodeDesc& node : nodes_) {
        if (node.id == 0 || !nodesById.emplace(node.id, &node).second) {
            setError(error, "Vulkan graph node IDs must be unique and nonzero");
            return false;
        }
        if (!parameterMatches(node)) {
            setError(error, "Vulkan graph node parameter type does not match its filter type");
            return false;
        }
    }

    const auto inputFound = nodesById.find(kVulkanGraphInputNodeId);
    const auto outputFound = nodesById.find(kVulkanGraphOutputNodeId);
    if (inputFound == nodesById.end()
        || inputFound->second->type != VulkanGraphNodeType::input
        || outputFound == nodesById.end()
        || outputFound->second->type != VulkanGraphNodeType::output) {
        setError(error, "Vulkan graph requires its fixed input and output nodes");
        return false;
    }

    std::unordered_map<VulkanGraphNodeId, int32_t> indegree;
    std::unordered_map<VulkanGraphNodeId, std::vector<VulkanGraphNodeId>> outgoing;
    std::unordered_map<VulkanGraphNodeId, std::vector<VulkanGraphNodeId>> incoming;
    std::unordered_set<NodePin, NodePinHash> occupiedInputs;
    for (const auto& [nodeId, node] : nodesById) {
        static_cast<void>(node);
        indegree[nodeId] = 0;
    }

    for (const GraphEdge& edge : edges_) {
        const auto source = nodesById.find(edge.output.nodeId);
        const auto destination = nodesById.find(edge.input.nodeId);
        if (source == nodesById.end() || destination == nodesById.end()
            || edge.output.nodeId == edge.input.nodeId) {
            setError(error, "Vulkan graph edge references an invalid node");
            return false;
        }
        const PinCounts sourcePins = pinCounts(*source->second);
        const PinCounts destinationPins = pinCounts(*destination->second);
        if (edge.output.pinIndex < 0
            || edge.output.pinIndex >= sourcePins.outputs
            || edge.input.pinIndex < 0
            || edge.input.pinIndex >= destinationPins.inputs) {
            setError(error, "Vulkan graph edge references an invalid pin");
            return false;
        }
        if (!occupiedInputs.insert(edge.input).second) {
            setError(error, "Vulkan graph input pin has more than one connection");
            return false;
        }
        ++indegree[edge.input.nodeId];
        outgoing[edge.output.nodeId].push_back(edge.input.nodeId);
        incoming[edge.input.nodeId].push_back(edge.output.nodeId);
    }

    for (const auto& [nodeId, node] : nodesById) {
        const PinCounts pins = pinCounts(*node);
        for (int32_t pin = 0; pin < pins.inputs; ++pin) {
            if (!occupiedInputs.contains({nodeId, pin})) {
                setError(error, "Vulkan graph has an unconnected input pin");
                return false;
            }
        }
    }

    std::queue<VulkanGraphNodeId> ready;
    for (const auto& [nodeId, count] : indegree) {
        if (count == 0) ready.push(nodeId);
    }
    size_t visitedCount = 0;
    while (!ready.empty()) {
        const VulkanGraphNodeId current = ready.front();
        ready.pop();
        ++visitedCount;
        for (VulkanGraphNodeId next : outgoing[current]) {
            if (--indegree[next] == 0) ready.push(next);
        }
    }
    if (visitedCount != nodes_.size()) {
        setError(error, "Vulkan graph contains a cycle");
        return false;
    }

    std::unordered_set<VulkanGraphNodeId> reachableFromInput;
    std::vector<VulkanGraphNodeId> pending{kVulkanGraphInputNodeId};
    while (!pending.empty()) {
        const VulkanGraphNodeId current = pending.back();
        pending.pop_back();
        if (!reachableFromInput.insert(current).second) continue;
        for (VulkanGraphNodeId next : outgoing[current]) pending.push_back(next);
    }

    std::unordered_set<VulkanGraphNodeId> reachesOutput;
    pending = {kVulkanGraphOutputNodeId};
    while (!pending.empty()) {
        const VulkanGraphNodeId current = pending.back();
        pending.pop_back();
        if (!reachesOutput.insert(current).second) continue;
        for (VulkanGraphNodeId previous : incoming[current]) {
            pending.push_back(previous);
        }
    }
    if (reachableFromInput.size() != nodes_.size()
        || reachesOutput.size() != nodes_.size()) {
        setError(error, "Every Vulkan graph node must be reachable from input and lead to output");
        return false;
    }
    return true;
}

} // namespace heisenberg::filtergraph
