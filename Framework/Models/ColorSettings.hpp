#pragma once

#include "Clip.hpp"
#include <map>

namespace heisenberg {

struct OutputSelection {
    std::string display = "sRGB - Display";
    std::string view = "Un-tone-mapped";
    bool operator==(const OutputSelection&) const = default;
};

// Persisted, value-only color contract shared by preview and offline export.
struct ColorSettings {
    int schemaVersion = 1;
    std::string configId = "studio-config-v4.0.0_aces-v2.0_ocio-v2.5";
    int inputPolicyVersion = 2; // 1: legacy camera interpretation; 2: finished SDR inputs
    std::map<std::string, std::string> inputColorSpaces; // mediaRef -> OCIO space
    OutputSelection previewOutput;
    OutputSelection exportOutput{"Rec.1886 Rec.709 - Display", "Un-tone-mapped"};
    int semanticsVersion = 2;
    // Missing legacy extension is an explicit, traceable migration, not silent metadata inference.
    bool legacyDefaultsApplied = false;
    JsonValue unknownFields = JsonValue::object();
    bool operator==(const ColorSettings&) const = default;
};

// Explicit pre-v2 defaults for persisted v1 extensions and legacy behavior tests.
ColorSettings legacyColorSettings();
// Execution/edit gate; validation alone intentionally permits unknown future versions for round trips.
bool supportedColorSettings(const ColorSettings& settings);
bool validateColorSettings(const ColorSettings& settings, std::string* error = nullptr);
JsonValue encodeColorSettings(const ColorSettings& settings);
bool decodeColorSettings(const JsonValue& value, ColorSettings& settings, std::string* error = nullptr);

} // namespace heisenberg
