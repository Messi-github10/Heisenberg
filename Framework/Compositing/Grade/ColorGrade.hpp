#pragma once

#include "GradeCompiler.hpp"
#include <array>
#include <optional>
#include <span>

namespace heisenberg::grade {

inline constexpr const char* kColorGradeType = "video.colorGrade";
inline constexpr int kColorGradeSchemaVersion = 1;
// OCIO conversions to/from this space belong to GradeCompiler, not the programs.
inline constexpr const char* kColorGradeAuxiliarySpace = "sRGB Encoded Rec.709 (sRGB)";
inline constexpr size_t kColorGradeMaxCurvePoints = 64;

struct ColorGradePoint {
    float x = 0, y = 0;
    bool operator==(const ColorGradePoint&) const = default;
};
using ColorGradeCurve = std::vector<ColorGradePoint>;
struct ColorGradeCurves {
    ColorGradeCurve master, red, green, blue;
    bool operator==(const ColorGradeCurves&) const = default;
};
struct ColorWheelState {
    float hue = 0, strength = 0, master = 0;
    bool operator==(const ColorWheelState&) const = default;
};
struct ColorGradeHueTarget {
    float hue = 0, rotation = 0, saturation = 1, lightness = 0;
    bool operator==(const ColorGradeHueTarget&) const = default;
};
// Reserved external-LUT selection. The LUT loader/compiler owns its resolution.
struct ColorGradeLutState {
    std::string id;
    std::string domain = kColorGradeAuxiliarySpace;
    float intensity = 1;
    bool operator==(const ColorGradeLutState&) const = default;
};
struct ColorGradeState {
    bool enabled = true;
    float ev = 0, contrast = 1;
    float highlights = 0, shadows = 0, blacks = 0, whites = 0;
    float temperature = 6500, tint = 0, vibrance = 0, saturation = 1;
    ColorWheelState lift{0, 0, 0}, gamma{0, 0, 1}, gain{0, 0, 1};
    ColorGradeCurves curves;
    std::vector<ColorGradeHueTarget> hueTargets;
    std::optional<ColorGradeLutState> lut;
    bool operator==(const ColorGradeState&) const = default;
};

struct ColorGradeParameterSpec {
    const char* name;
    float minimum, maximum, defaultValue;
};
std::span<const ColorGradeParameterSpec> colorGradeParameterRegistry();

// Transactional: failure leaves output unchanged; diagnostics are appended.
// Every persisted base/key value is validated, including currently inactive keys.
bool sampleColorGrade(const JsonValue& effect, int64_t clipLocalFrame,
                      ColorGradeState& output, std::vector<GradeDiagnostic>& diagnostics,
                      std::string* error = nullptr, GradeCompileOptions options = {});

// Pure edit normalization: preserves unknown fields and all tracks, adds defaults,
// normalizes hue 360 to 0, and omits full-range identity curves. Does not sample or
// erase animated parameters just because their base value is neutral.
bool canonicalizeColorGrade(const JsonValue& effect, JsonValue& output,
                            std::string* error = nullptr);

// Also validates direct strongly-typed API users before GPU resources are built.
bool validateColorGradeState(const ColorGradeState& state, std::string* error = nullptr);
bool isIdentityColorGradeCurve(const ColorGradeCurve& curve);

} // namespace heisenberg::grade
