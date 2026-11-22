#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace heisenberg {

enum class BlendMode {
    Normal = 0,
    Darken,
    Multiply,
    ColorBurn,
    Lighten,
    Screen,
    ColorDodge,
    Overlay,
    SoftLight,
    HardLight,
    Difference,
    Exclusion,
    Hue,
    Saturation,
    Color,
    Luminosity,
    Add
};

struct BlendModeDescriptor {
    BlendMode mode;
    std::string_view name;
    std::string_view displayName;
};

std::span<const BlendModeDescriptor> blendModes();
// An absent stored value means Normal; explicit empty/unknown values are invalid.
std::optional<BlendMode> parseBlendMode(const std::optional<std::string>& name);
// Returns an empty view for an invalid enum value.
std::string_view blendModeName(BlendMode mode);

} // namespace heisenberg
