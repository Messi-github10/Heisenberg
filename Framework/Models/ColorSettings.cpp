#include "ColorSettings.hpp"
#include <exception>
#include <stdexcept>

namespace heisenberg {

namespace {

void setError(std::string* error, const std::string& value) {
    if (error) *error = value;
}

JsonValue outputJson(JsonValue value, const OutputSelection& output) {
    if (!value.is_object()) value = JsonValue::object();
    value["display"] = output.display;
    value["view"] = output.view;
    return value;
}

}

ColorSettings legacyColorSettings() {
    ColorSettings settings;
    settings.inputPolicyVersion = 1;
    settings.semanticsVersion = 1;
    settings.previewOutput.view = "ACES 2.0 - SDR 100 nits (Rec.709)";
    settings.exportOutput.view = "ACES 2.0 - SDR 100 nits (Rec.709)";
    return settings;
}

bool supportedColorSettings(const ColorSettings& settings) {
    return settings.schemaVersion == 1 &&
        ((settings.inputPolicyVersion == 1 && settings.semanticsVersion == 1) ||
         (settings.inputPolicyVersion == 2 && settings.semanticsVersion == 2));
}

bool validateColorSettings(const ColorSettings& value, std::string* error) {
    if (value.schemaVersion <= 0 || value.inputPolicyVersion <= 0 || value.semanticsVersion <= 0 ||
        value.configId.empty() || value.previewOutput.display.empty() || value.previewOutput.view.empty() ||
        value.exportOutput.display.empty() || value.exportOutput.view.empty() || !value.unknownFields.is_object()) {
        setError(error, "color settings versions and output selections must be valid"); return false;
    }
    for (const auto& [ref, space] : value.inputColorSpaces) {
        if (ref.empty() || space.empty()) { setError(error, "input color overrides must have a mediaRef and color space"); return false; }
    }
    return true;
}

JsonValue encodeColorSettings(const ColorSettings& value) {
    JsonValue result = value.unknownFields;
    result["schemaVersion"] = value.schemaVersion;
    result["configId"] = value.configId;
    result["inputPolicyVersion"] = value.inputPolicyVersion;
    result["inputColorSpaces"] = value.inputColorSpaces;
    result["previewOutput"] = outputJson(result.value("previewOutput", JsonValue::object()), value.previewOutput);
    result["exportOutput"] = outputJson(result.value("exportOutput", JsonValue::object()), value.exportOutput);
    result["semanticsVersion"] = value.semanticsVersion;
    result["legacyDefaultsApplied"] = value.legacyDefaultsApplied;
    return result;
}

bool decodeColorSettings(const JsonValue& value, ColorSettings& result, std::string* error) {
    try {
        if (!value.is_object()) { setError(error, "color extension must be an object"); return false; }
        const int policy = value.value("inputPolicyVersion", 1);
        const int semantics = value.value("semanticsVersion", 1);
        ColorSettings parsed = policy == 2 && semantics == 2 ? ColorSettings{} : legacyColorSettings();
        parsed.schemaVersion = value.value("schemaVersion", 1);
        parsed.configId = value.value("configId", parsed.configId);
        parsed.inputPolicyVersion = policy;
        parsed.semanticsVersion = semantics;
        parsed.legacyDefaultsApplied = value.value("legacyDefaultsApplied", false);
        if (value.contains("inputColorSpaces")) parsed.inputColorSpaces = value.at("inputColorSpaces").get<std::map<std::string, std::string>>();
        const auto parseOutput = [&](const char* key, OutputSelection& output) {
            if (!value.contains(key)) return;
            const auto& item = value.at(key);
            if (!item.is_object()) throw std::runtime_error("output selection must be an object");
            output.display = item.value("display", output.display);
            output.view = item.value("view", output.view);
        };
        parseOutput("previewOutput", parsed.previewOutput);
        parseOutput("exportOutput", parsed.exportOutput);
        parsed.unknownFields = value;
        for (const char* key : {"schemaVersion", "configId", "inputPolicyVersion", "inputColorSpaces", "semanticsVersion", "legacyDefaultsApplied"})
            parsed.unknownFields.erase(key);
        for (const char* key : {"previewOutput", "exportOutput"}) {
            if (!parsed.unknownFields.contains(key)) continue;
            parsed.unknownFields[key].erase("display");
            parsed.unknownFields[key].erase("view");
            if (parsed.unknownFields[key].empty()) parsed.unknownFields.erase(key);
        }
        if (!validateColorSettings(parsed, error)) return false;
        result = std::move(parsed);
        return true;
    } catch (const std::exception& exception) {
        setError(error, std::string("invalid color settings: ") + exception.what()); return false;
    }
}

} // namespace heisenberg
