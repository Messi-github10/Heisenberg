#include "VulkanFilterRegistry.hpp"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <variant>

#ifndef HEISENBERG_SHADER_MANIFEST_PATH
#define HEISENBERG_SHADER_MANIFEST_PATH "shader_manifest.json"
#endif

namespace heisenberg::filtergraph {
namespace {

using json = nlohmann::json;

void setError(std::string* error, const std::string& message) {
    if (error) *error = message;
}

const json& jsonField(const json& object, const char* key) {
    static const json missing;
    const auto it = object.find(key);
    return it == object.end() ? missing : *it;
}

const json& jsonArrayField(const json& object, const char* key) {
    static const json empty = json::array();
    const json& value = jsonField(object, key);
    return value.is_array() ? value : empty;
}

const json& jsonObjectField(const json& object, const char* key) {
    static const json empty = json::object();
    const json& value = jsonField(object, key);
    return value.is_object() ? value : empty;
}

std::string jsonString(const json& value, const std::string& fallback = {}) {
    return value.is_string() ? value.get<std::string>() : fallback;
}

std::string jsonStringField(const json& object, const char* key,
                            const std::string& fallback = {}) {
    return jsonString(jsonField(object, key), fallback);
}

int jsonInt(const json& value, int fallback = 0) {
    if (!value.is_number()) return fallback;
    return static_cast<int>(value.get<double>());
}

int jsonIntField(const json& object, const char* key, int fallback = 0) {
    return jsonInt(jsonField(object, key), fallback);
}

bool jsonBool(const json& value, bool fallback = false) {
    return value.is_boolean() ? value.get<bool>() : fallback;
}

bool jsonBoolField(const json& object, const char* key, bool fallback = false) {
    return jsonBool(jsonField(object, key), fallback);
}

double jsonDoubleField(const json& object, const char* key, double fallback = 0.0) {
    const json& value = jsonField(object, key);
    return value.is_number() ? value.get<double>() : fallback;
}

bool finiteNumber(const json& value, double& result) {
    if (!value.is_number()) return false;
    result = value.get<double>();
    return std::isfinite(result);
}

bool readInt(const json& object, const char* key, int32_t minimum,
             int32_t& result, std::string* error) {
    const json& value = jsonField(object, key);
    double number = 0.0;
    if (!finiteNumber(value, number) || std::floor(number) != number
        || number < minimum || number > std::numeric_limits<int32_t>::max()) {
        setError(error, std::string("Manifest field '") + key + "' must be an integer");
        return false;
    }
    result = static_cast<int32_t>(number);
    return true;
}

bool parseKind(const std::string& value, VulkanFilterKind& result) {
    if (value == "compute") result = VulkanFilterKind::compute;
    else if (value == "multi_pass") result = VulkanFilterKind::multiPass;
    else if (value == "stateful") result = VulkanFilterKind::stateful;
    else if (value == "readback") result = VulkanFilterKind::readback;
    else if (value == "input") result = VulkanFilterKind::input;
    else if (value == "output") result = VulkanFilterKind::output;
    else return false;
    return true;
}

bool parseValueType(const std::string& value, VulkanFilterValueType& result) {
    if (value == "int") result = VulkanFilterValueType::integer;
    else if (value == "float") result = VulkanFilterValueType::real;
    else if (value == "bool") result = VulkanFilterValueType::boolean;
    else return false;
    return true;
}

VulkanInputBinding parseBinding(const std::string& value) {
    if (value == "sampled_linear") {
        return VulkanInputBinding::sampledLinear;
    }
    if (value == "sampled_nearest") {
        return VulkanInputBinding::sampledNearest;
    }
    return VulkanInputBinding::storageImage;
}

} // namespace

VulkanFilterRegistry& VulkanFilterRegistry::instance() {
    static VulkanFilterRegistry registry;
    return registry;
}

bool VulkanFilterRegistry::ensureLoaded(std::string* error) {
    return loaded_ || load(error);
}

bool VulkanFilterRegistry::load(std::string* error) {
    std::ifstream file(std::filesystem::u8path(HEISENBERG_SHADER_MANIFEST_PATH),
                       std::ios::binary);
    if (!file) {
        setError(error, std::string("Failed to open shader manifest '")
            + HEISENBERG_SHADER_MANIFEST_PATH + "'");
        return false;
    }

    json document;
    try {
        document = json::parse(file);
    } catch (const json::parse_error& parseError) {
        setError(error, std::string("Invalid shader manifest: ") + parseError.what());
        return false;
    }
    if (!document.is_object()) {
        setError(error, "Invalid shader manifest: root must be an object");
        return false;
    }

    const json& filters = jsonArrayField(document, "shaders");
    if (filters.empty()) {
        setError(error, "Shader manifest contains no shaders");
        return false;
    }

    std::vector<VulkanFilterDescriptor> parsed;
    for (const json& value : filters) {
        if (!value.is_object()) {
            setError(error, "Every shader manifest entry must be an object");
            return false;
        }
        const json& object = value;
        VulkanFilterDescriptor descriptor;
        descriptor.id = jsonStringField(object, "id", jsonStringField(object, "node"));
        descriptor.displayName = jsonStringField(object, "name", descriptor.id);
        if (descriptor.id.empty()) {
            setError(error, "Shader manifest entry has no id");
            return false;
        }
        if (!parseKind(jsonStringField(object, "kind", "compute"), descriptor.kind)) {
            setError(error, "Unknown shader manifest kind for '" + descriptor.id + "'");
            return false;
        }
        descriptor.shaderSource = jsonStringField(object, "source");
        descriptor.shaderBinary = jsonStringField(object, "spirv");
        if (!readInt(object, "inputs", 0, descriptor.inputCount, error)
            || !readInt(object, "outputs", 0, descriptor.outputCount, error)) {
            return false;
        }
        if ((descriptor.kind == VulkanFilterKind::compute
             || descriptor.kind == VulkanFilterKind::multiPass
             || descriptor.kind == VulkanFilterKind::stateful)
            && (descriptor.inputCount < 1 || descriptor.outputCount < 1)) {
            setError(error, "Executable filter '" + descriptor.id
                + "' must have at least one input and output");
            return false;
        }
        for (const json& binding : jsonArrayField(object, "input_bindings")) {
            descriptor.inputBindings.push_back(parseBinding(jsonString(binding)));
        }
        while (descriptor.inputBindings.size() < static_cast<size_t>(descriptor.inputCount)) {
            descriptor.inputBindings.push_back(VulkanInputBinding::storageImage);
        }
        for (const json& extraValue : jsonArrayField(object, "extra_inputs")) {
            const json& extraObject = extraValue.is_object() ? extraValue : json::object();
            VulkanFilterExtraInputDescriptor extra;
            extra.name = jsonStringField(extraObject, "name");
            extra.binding = parseBinding(jsonStringField(extraObject, "binding_type",
                                                         "sampled_linear"));
            if (extra.name.empty()) {
                setError(error, "Invalid extra input in shader manifest entry '"
                    + descriptor.id + "'");
                return false;
            }
            descriptor.extraInputs.push_back(std::move(extra));
        }
        descriptor.auxiliarySource = jsonStringField(object, "auxiliary_source");
        const int auxiliaryWidth = jsonIntField(object, "auxiliary_width");
        const int auxiliaryHeight = jsonIntField(object, "auxiliary_height");
        if (auxiliaryWidth < 0 || auxiliaryHeight < 0) {
            setError(error, "Auxiliary texture dimensions must be nonnegative");
            return false;
        }
        descriptor.auxiliaryWidth = static_cast<uint32_t>(auxiliaryWidth);
        descriptor.auxiliaryHeight = static_cast<uint32_t>(auxiliaryHeight);
        if (descriptor.auxiliarySource == "identity_lut"
            && (descriptor.auxiliaryWidth != 512
                || descriptor.auxiliaryHeight != 512)) {
            setError(error, "identity_lut requires a 512x512 auxiliary texture");
            return false;
        }
        const int uniformSize = jsonIntField(object, "uniform_size");
        if (uniformSize < 0) {
            setError(error, "Manifest uniform_size must be nonnegative");
            return false;
        }
        descriptor.uniformSize = static_cast<size_t>(uniformSize);
        descriptor.resizeOutput = jsonStringField(object, "output_size") == "parameters";
        descriptor.passthroughOutput = jsonStringField(object, "output") == "passthrough";
        if (descriptor.kind == VulkanFilterKind::readback) {
            const json& readback = jsonObjectField(object, "readback");
            const int readbackSize = jsonIntField(readback, "size");
            const int readbackBinding = jsonIntField(readback, "binding", -1);
            if (readbackSize <= 0 || readbackBinding < 0) {
                setError(error, "Readback filter '" + descriptor.id
                    + "' must declare a positive size and binding");
                return false;
            }
            descriptor.readbackSize = static_cast<size_t>(readbackSize);
            descriptor.readbackBinding = static_cast<uint32_t>(readbackBinding);
            descriptor.clearReadbackBuffer = jsonBoolField(readback, "clear");
            if (descriptor.readbackBinding
                < static_cast<uint32_t>(descriptor.inputCount)) {
                setError(error, "Readback binding for '" + descriptor.id
                    + "' must follow all input bindings");
                return false;
            }
        }

        for (const json& passValue : jsonArrayField(object, "passes")) {
            const json& passObject = passValue.is_object() ? passValue : json::object();
            VulkanFilterPassDescriptor pass;
            pass.name = jsonStringField(passObject, "name");
            const json& direction = jsonArrayField(passObject, "direction");
            if (pass.name.empty() || direction.size() != 2
                || !direction[0].is_number() || !direction[1].is_number()
                || std::floor(direction[0].get<double>()) != direction[0].get<double>()
                || std::floor(direction[1].get<double>()) != direction[1].get<double>()) {
                setError(error, "Invalid pass descriptor in shader manifest entry '"
                    + descriptor.id + "'");
                return false;
            }
            pass.directionX = jsonInt(direction[0]);
            pass.directionY = jsonInt(direction[1]);
            descriptor.passes.push_back(std::move(pass));
        }
        if (descriptor.kind == VulkanFilterKind::multiPass
            && descriptor.passes.empty()) {
            setError(error, "Multi-pass filter '" + descriptor.id + "' must declare passes");
            return false;
        }

        for (const json& parameterValue : jsonArrayField(object, "parameters")) {
            const json& parameterObject =
                parameterValue.is_object() ? parameterValue : json::object();
            VulkanFilterParameterDesc parameter;
            parameter.name = jsonStringField(parameterObject, "name");
            if (parameter.name.empty()
                || !parseValueType(jsonStringField(parameterObject, "type"), parameter.type)) {
                setError(error, "Invalid parameter in shader manifest entry '"
                    + descriptor.id + "'");
                return false;
            }
            parameter.offset = static_cast<size_t>(jsonIntField(parameterObject, "offset"));
            parameter.defaultValue = jsonDoubleField(parameterObject, "default");
            parameter.exposed = jsonBoolField(parameterObject, "exposed", true);
            if (parameterObject.contains("min")) {
                parameter.hasMinimum = finiteNumber(parameterObject.at("min"), parameter.minimum);
            }
            if (parameterObject.contains("max")) {
                parameter.hasMaximum = finiteNumber(parameterObject.at("max"), parameter.maximum);
            }
            descriptor.parameters.push_back(std::move(parameter));
        }
        if (!parsed.empty() && std::any_of(parsed.begin(), parsed.end(),
            [&descriptor](const VulkanFilterDescriptor& item) { return item.id == descriptor.id; })) {
            setError(error, "Duplicate shader manifest id '" + descriptor.id + "'");
            return false;
        }
        parsed.push_back(std::move(descriptor));
    }
    descriptors_ = std::move(parsed);
    loaded_ = true;
    return true;
}

const VulkanFilterDescriptor* VulkanFilterRegistry::find(
    std::string_view id, std::string* error) {
    if (!ensureLoaded(error)) return nullptr;
    const auto found = std::find_if(descriptors_.begin(), descriptors_.end(),
        [id](const VulkanFilterDescriptor& descriptor) { return descriptor.id == id; });
    return found == descriptors_.end() ? nullptr : &*found;
}

VulkanGraphParameter VulkanFilterRegistry::defaultParameters(
    const VulkanFilterDescriptor& descriptor) const {
    VulkanGraphParameter result;
    for (const VulkanFilterParameterDesc& field : descriptor.parameters) {
        if (!field.exposed) continue;
        switch (field.type) {
            case VulkanFilterValueType::integer:
                result.emplace(field.name, static_cast<int32_t>(field.defaultValue));
                break;
            case VulkanFilterValueType::real:
                result.emplace(field.name, static_cast<float>(field.defaultValue));
                break;
            case VulkanFilterValueType::boolean:
                result.emplace(field.name, field.defaultValue != 0.0);
                break;
        }
    }
    return result;
}

bool VulkanFilterRegistry::parseParameters(
    const VulkanFilterDescriptor& descriptor, const nlohmann::json& object,
    VulkanGraphParameter& result, std::string* error) const {
    result = defaultParameters(descriptor);
    if (!object.is_object()) return validateParameters(descriptor, result, error);
    for (auto it = object.begin(); it != object.end(); ++it) {
        const std::string name = it.key();
        const auto field = std::find_if(
            descriptor.parameters.begin(), descriptor.parameters.end(),
            [&](const VulkanFilterParameterDesc& item) { return item.name == name; });
        if (field == descriptor.parameters.end() || !field->exposed) continue;

        const json& value = it.value();
        double number = 0.0;
        switch (field->type) {
            case VulkanFilterValueType::integer:
                if (!finiteNumber(value, number) || std::floor(number) != number
                    || number < std::numeric_limits<int32_t>::min()
                    || number > std::numeric_limits<int32_t>::max()) {
                    if (error) *error = "Invalid filter parameter: " + name;
                    return false;
                }
                result[name] = static_cast<int32_t>(number);
                break;
            case VulkanFilterValueType::real:
                if (!finiteNumber(value, number)) {
                    if (error) *error = "Invalid filter parameter: " + name;
                    return false;
                }
                result[name] = static_cast<float>(number);
                break;
            case VulkanFilterValueType::boolean:
                if (!value.is_boolean()) {
                    if (error) *error = "Invalid filter parameter: " + name;
                    return false;
                }
                result[name] = value.get<bool>();
                break;
        }
    }
    return validateParameters(descriptor, result, error);
}

bool VulkanFilterRegistry::validateParameters(
    const VulkanFilterDescriptor& descriptor,
    const VulkanGraphParameter& parameter, std::string* error) const {
    for (const VulkanFilterParameterDesc& field : descriptor.parameters) {
        if (!field.exposed) continue;
        const auto found = parameter.find(field.name);
        if (found == parameter.end()) {
            if (error) *error = "Missing filter parameter: " + field.name;
            return false;
        }

        double number = 0.0;
        switch (field.type) {
            case VulkanFilterValueType::integer: {
                const auto* value = std::get_if<int32_t>(&found->second);
                if (!value) {
                    if (error) *error = "Invalid filter parameter: " + field.name;
                    return false;
                }
                number = *value;
                break;
            }
            case VulkanFilterValueType::real: {
                const auto* value = std::get_if<float>(&found->second);
                if (!value) {
                    if (error) *error = "Invalid filter parameter: " + field.name;
                    return false;
                }
                number = *value;
                break;
            }
            case VulkanFilterValueType::boolean:
                if (!std::holds_alternative<bool>(found->second)) {
                    if (error) *error = "Invalid filter parameter: " + field.name;
                    return false;
                }
                continue;
        }
        if (field.hasMinimum && number < field.minimum) {
            if (error) *error = "Filter parameter below minimum: " + field.name;
            return false;
        }
        if (field.hasMaximum && number > field.maximum) {
            if (error) *error = "Filter parameter above maximum: " + field.name;
            return false;
        }
    }
    return true;
}

} // namespace heisenberg::filtergraph
