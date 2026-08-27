#include "VulkanFilterRegistry.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QString>
#include <algorithm>
#include <cmath>
#include <limits>
#include <iterator>

#ifndef HEISENBERG_SHADER_MANIFEST_PATH
#define HEISENBERG_SHADER_MANIFEST_PATH "shader_manifest.json"
#endif

namespace heisenberg::filtergraph {
namespace {

void setError(std::string* error, const QString& message) {
    if (error) *error = message.toStdString();
}

bool finiteNumber(const QJsonValue& value, double& result) {
    if (!value.isDouble()) return false;
    result = value.toDouble(std::numeric_limits<double>::quiet_NaN());
    return std::isfinite(result);
}

bool readInt(const QJsonObject& object, const char* key, int32_t minimum,
             int32_t& result, std::string* error) {
    const QJsonValue value = object.value(QLatin1String(key));
    double number = 0.0;
    if (!finiteNumber(value, number) || std::floor(number) != number
        || number < minimum || number > std::numeric_limits<int32_t>::max()) {
        setError(error, QStringLiteral("Manifest field '%1' must be an integer")
            .arg(QLatin1String(key)));
        return false;
    }
    result = static_cast<int32_t>(number);
    return true;
}

bool parseKind(const QString& value, VulkanFilterKind& result) {
    if (value == QLatin1String("compute")) result = VulkanFilterKind::compute;
    else if (value == QLatin1String("multi_pass")) result = VulkanFilterKind::multiPass;
    else if (value == QLatin1String("stateful")) result = VulkanFilterKind::stateful;
    else if (value == QLatin1String("readback")) result = VulkanFilterKind::readback;
    else if (value == QLatin1String("input")) result = VulkanFilterKind::input;
    else if (value == QLatin1String("output")) result = VulkanFilterKind::output;
    else return false;
    return true;
}

bool parseValueType(const QString& value, VulkanFilterValueType& result) {
    if (value == QLatin1String("int")) result = VulkanFilterValueType::integer;
    else if (value == QLatin1String("float")) result = VulkanFilterValueType::real;
    else if (value == QLatin1String("bool")) result = VulkanFilterValueType::boolean;
    else return false;
    return true;
}

VulkanInputBinding parseBinding(const QString& value) {
    if (value == QLatin1String("sampled_linear")) {
        return VulkanInputBinding::sampledLinear;
    }
    if (value == QLatin1String("sampled_nearest")) {
        return VulkanInputBinding::sampledNearest;
    }
    return VulkanInputBinding::storageImage;
}

} // namespace

const char* vulkanGraphNodeTypeName(VulkanGraphNodeType type) noexcept {
    constexpr const char* names[] = {
        "input", "output", "color_invert", "exposure", "blend",
        "gaussian_blur", "resize", "lut", "histogram",
    };
    const auto index = static_cast<size_t>(type);
    return index < std::size(names) ? names[index] : "";
}

VulkanFilterRegistry& VulkanFilterRegistry::instance() {
    static VulkanFilterRegistry registry;
    return registry;
}

bool VulkanFilterRegistry::ensureLoaded(std::string* error) {
    return loaded_ || load(error);
}

bool VulkanFilterRegistry::load(std::string* error) {
    QFile file(QString::fromUtf8(HEISENBERG_SHADER_MANIFEST_PATH));
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, QStringLiteral("Failed to open shader manifest '%1': %2")
            .arg(file.fileName(), file.errorString()));
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        setError(error, QStringLiteral("Invalid shader manifest: %1")
            .arg(parseError.errorString()));
        return false;
    }
    const QJsonArray filters = document.object().value("shaders").toArray();
    if (filters.isEmpty()) {
        setError(error, QStringLiteral("Shader manifest contains no shaders"));
        return false;
    }

    std::vector<VulkanFilterDescriptor> parsed;
    for (const QJsonValue& value : filters) {
        if (!value.isObject()) {
            setError(error, QStringLiteral("Every shader manifest entry must be an object"));
            return false;
        }
        const QJsonObject object = value.toObject();
        VulkanFilterDescriptor descriptor;
        descriptor.id = object.value("id").toString(object.value("node").toString()).toStdString();
        descriptor.displayName = object.value("name").toString(
            QString::fromStdString(descriptor.id)).toStdString();
        if (descriptor.id.empty()) {
            setError(error, QStringLiteral("Shader manifest entry has no id"));
            return false;
        }
        if (!parseKind(object.value("kind").toString("compute"), descriptor.kind)) {
            setError(error, QStringLiteral("Unknown shader manifest kind for '%1'")
                .arg(QString::fromStdString(descriptor.id)));
            return false;
        }
        descriptor.shaderSource = object.value("source").toString().toStdString();
        descriptor.shaderBinary = object.value("spirv").toString().toStdString();
        if (!readInt(object, "inputs", 0, descriptor.inputCount, error)
            || !readInt(object, "outputs", 0, descriptor.outputCount, error)) {
            return false;
        }
        if ((descriptor.kind == VulkanFilterKind::compute
             || descriptor.kind == VulkanFilterKind::multiPass
             || descriptor.kind == VulkanFilterKind::stateful)
            && (descriptor.inputCount < 1 || descriptor.outputCount < 1)) {
            setError(error, QStringLiteral("Executable filter '%1' must have at least one input and output")
                .arg(QString::fromStdString(descriptor.id)));
            return false;
        }
        const QJsonArray bindings = object.value("input_bindings").toArray();
        for (const QJsonValue& binding : bindings) {
            descriptor.inputBindings.push_back(parseBinding(binding.toString()));
        }
        while (descriptor.inputBindings.size() < static_cast<size_t>(descriptor.inputCount)) {
            descriptor.inputBindings.push_back(VulkanInputBinding::storageImage);
        }
        for (const QJsonValue& extraValue : object.value("extra_inputs").toArray()) {
            const QJsonObject extraObject = extraValue.toObject();
            VulkanFilterExtraInputDescriptor extra;
            extra.name = extraObject.value("name").toString().toStdString();
            extra.binding = parseBinding(extraObject.value("binding_type")
                .toString("sampled_linear"));
            if (extra.name.empty()) {
                setError(error, QStringLiteral("Invalid extra input in shader manifest entry '%1'")
                    .arg(QString::fromStdString(descriptor.id)));
                return false;
            }
            descriptor.extraInputs.push_back(std::move(extra));
        }
        descriptor.auxiliarySource = object.value("auxiliary_source")
            .toString().toStdString();
        const int auxiliaryWidth = object.value("auxiliary_width").toInt(0);
        const int auxiliaryHeight = object.value("auxiliary_height").toInt(0);
        if (auxiliaryWidth < 0 || auxiliaryHeight < 0) {
            setError(error, QStringLiteral("Auxiliary texture dimensions must be nonnegative"));
            return false;
        }
        descriptor.auxiliaryWidth = static_cast<uint32_t>(auxiliaryWidth);
        descriptor.auxiliaryHeight = static_cast<uint32_t>(auxiliaryHeight);
        if (descriptor.auxiliarySource == "identity_lut"
            && (descriptor.auxiliaryWidth != 512
                || descriptor.auxiliaryHeight != 512)) {
            setError(error, QStringLiteral("identity_lut requires a 512x512 auxiliary texture"));
            return false;
        }
        const int uniformSize = object.value("uniform_size").toInt(0);
        if (uniformSize < 0) {
            setError(error, QStringLiteral("Manifest uniform_size must be nonnegative"));
            return false;
        }
        descriptor.uniformSize = static_cast<size_t>(uniformSize);
        descriptor.resizeOutput = object.value("output_size").toString()
            == QLatin1String("parameters");
        descriptor.passthroughOutput = object.value("output").toString()
            == QLatin1String("passthrough");
        if (descriptor.kind == VulkanFilterKind::readback) {
            const QJsonObject readback = object.value("readback").toObject();
            const int readbackSize = readback.value("size").toInt(0);
            const int readbackBinding = readback.value("binding").toInt(-1);
            if (readbackSize <= 0 || readbackBinding < 0) {
                setError(error, QStringLiteral(
                    "Readback filter '%1' must declare a positive size and binding")
                    .arg(QString::fromStdString(descriptor.id)));
                return false;
            }
            descriptor.readbackSize = static_cast<size_t>(readbackSize);
            descriptor.readbackBinding = static_cast<uint32_t>(readbackBinding);
            descriptor.clearReadbackBuffer = readback.value("clear").toBool(false);
            if (descriptor.readbackBinding
                < static_cast<uint32_t>(descriptor.inputCount)) {
                setError(error, QStringLiteral(
                    "Readback binding for '%1' must follow all input bindings")
                    .arg(QString::fromStdString(descriptor.id)));
                return false;
            }
        }

        for (const QJsonValue& passValue : object.value("passes").toArray()) {
            const QJsonObject passObject = passValue.toObject();
            VulkanFilterPassDescriptor pass;
            pass.name = passObject.value("name").toString().toStdString();
            const QJsonArray direction = passObject.value("direction").toArray();
            if (pass.name.empty() || direction.size() != 2
                || !direction[0].isDouble() || !direction[1].isDouble()
                || std::floor(direction[0].toDouble()) != direction[0].toDouble()
                || std::floor(direction[1].toDouble()) != direction[1].toDouble()) {
                setError(error, QStringLiteral("Invalid pass descriptor in shader manifest entry '%1'")
                    .arg(QString::fromStdString(descriptor.id)));
                return false;
            }
            pass.directionX = direction[0].toInt();
            pass.directionY = direction[1].toInt();
            descriptor.passes.push_back(std::move(pass));
        }
        if (descriptor.kind == VulkanFilterKind::multiPass
            && descriptor.passes.empty()) {
            setError(error, QStringLiteral("Multi-pass filter '%1' must declare passes")
                .arg(QString::fromStdString(descriptor.id)));
            return false;
        }

        for (const QJsonValue& parameterValue : object.value("parameters").toArray()) {
            const QJsonObject parameterObject = parameterValue.toObject();
            VulkanFilterParameterDesc parameter;
            parameter.name = parameterObject.value("name").toString().toStdString();
            if (parameter.name.empty()
                || !parseValueType(parameterObject.value("type").toString(), parameter.type)) {
                setError(error, QStringLiteral("Invalid parameter in shader manifest entry '%1'")
                    .arg(QString::fromStdString(descriptor.id)));
                return false;
            }
            parameter.offset = static_cast<size_t>(parameterObject.value("offset").toInt(0));
            parameter.defaultValue = parameterObject.value("default").toDouble(0.0);
            parameter.exposed = parameterObject.value("exposed").toBool(true);
            if (parameterObject.contains("min")) {
                parameter.hasMinimum = finiteNumber(parameterObject.value("min"), parameter.minimum);
            }
            if (parameterObject.contains("max")) {
                parameter.hasMaximum = finiteNumber(parameterObject.value("max"), parameter.maximum);
            }
            descriptor.parameters.push_back(std::move(parameter));
        }
        if (!parsed.empty() && std::any_of(parsed.begin(), parsed.end(),
            [&descriptor](const VulkanFilterDescriptor& item) { return item.id == descriptor.id; })) {
            setError(error, QStringLiteral("Duplicate shader manifest id '%1'")
                .arg(QString::fromStdString(descriptor.id)));
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

const VulkanFilterDescriptor* VulkanFilterRegistry::find(
    VulkanGraphNodeType type, std::string* error) {
    return find(vulkanGraphNodeTypeName(type), error);
}

bool VulkanFilterRegistry::parseParameters(
    const VulkanFilterDescriptor& descriptor, const QJsonObject& object,
    VulkanGraphParameter& result, std::string* error) const {
    QJsonObject normalized = object;
    for (const VulkanFilterParameterDesc& parameter : descriptor.parameters) {
        if (parameter.exposed
            && !normalized.contains(QString::fromStdString(parameter.name))) {
            switch (parameter.type) {
                case VulkanFilterValueType::integer:
                    normalized.insert(QString::fromStdString(parameter.name),
                        static_cast<qint64>(parameter.defaultValue));
                    break;
                case VulkanFilterValueType::real:
                    normalized.insert(QString::fromStdString(parameter.name), parameter.defaultValue);
                    break;
                case VulkanFilterValueType::boolean:
                    normalized.insert(QString::fromStdString(parameter.name), parameter.defaultValue != 0.0);
                    break;
            }
        }
    }
    result = VulkanJsonParameter{std::move(normalized)};
    return validateParameters(descriptor, result, error);
}

bool VulkanFilterRegistry::validateParameters(
    const VulkanFilterDescriptor& descriptor,
    const VulkanGraphParameter& parameter, std::string* error) const {
    QJsonObject object;
    if (const auto* json = std::get_if<VulkanJsonParameter>(&parameter)) {
        object = json->object;
    }
    for (const VulkanFilterParameterDesc& field : descriptor.parameters) {
        if (!field.exposed) continue;
        const QJsonValue value = object.value(QString::fromStdString(field.name));
        double number = 0.0;
        if (field.type != VulkanFilterValueType::boolean
            && (!finiteNumber(value, number)
                || (field.type == VulkanFilterValueType::integer && std::floor(number) != number))) {
            if (error) *error = "Invalid filter parameter: " + field.name;
            return false;
        }
        if (field.type == VulkanFilterValueType::boolean && !value.isBool()) {
            if (error) *error = "Invalid filter parameter: " + field.name;
            return false;
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
