#pragma once

#include "Clip.hpp"
#include <map>

namespace heisenberg {

struct OutputSelection {
    std::string display = "sRGB - Display";
    std::string view = "Un-tone-mapped";
    bool operator==(const OutputSelection&) const = default;
};

struct ColorSettings {
    int schemaVersion = 1;
    std::string configId = "studio-config-v4.0.0_aces-v2.0_ocio-v2.5";
    int inputPolicyVersion = 2;
    std::map<std::string, std::string> inputColorSpaces;
    OutputSelection previewOutput;
    OutputSelection exportOutput{"Rec.1886 Rec.709 - Display", "Un-tone-mapped"};
    int semanticsVersion = 2;
    bool legacyDefaultsApplied = false;
    JsonValue unknownFields = JsonValue::object();
    bool operator==(const ColorSettings&) const = default;
};

ColorSettings legacyColorSettings();
bool supportedColorSettings(const ColorSettings& settings);
bool validateColorSettings(const ColorSettings& settings, std::string* error = nullptr);
JsonValue encodeColorSettings(const ColorSettings& settings);
bool decodeColorSettings(const JsonValue& value, ColorSettings& settings, std::string* error = nullptr);

} // namespace heisenberg
