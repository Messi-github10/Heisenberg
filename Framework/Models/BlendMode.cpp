#include <Models/BlendMode.hpp>

#include <array>

namespace heisenberg {
namespace {
constexpr std::array<BlendModeDescriptor, 17> modes{{
    {BlendMode::Normal, "normal", "Normal"},
    {BlendMode::Darken, "darken", "Darken"},
    {BlendMode::Multiply, "multiply", "Multiply"},
    {BlendMode::ColorBurn, "colorBurn", "Color Burn"},
    {BlendMode::Lighten, "lighten", "Lighten"},
    {BlendMode::Screen, "screen", "Screen"},
    {BlendMode::ColorDodge, "colorDodge", "Color Dodge"},
    {BlendMode::Overlay, "overlay", "Overlay"},
    {BlendMode::SoftLight, "softLight", "Soft Light"},
    {BlendMode::HardLight, "hardLight", "Hard Light"},
    {BlendMode::Difference, "difference", "Difference"},
    {BlendMode::Exclusion, "exclusion", "Exclusion"},
    {BlendMode::Hue, "hue", "Hue"},
    {BlendMode::Saturation, "saturation", "Saturation"},
    {BlendMode::Color, "color", "Color"},
    {BlendMode::Luminosity, "luminosity", "Luminosity"},
    {BlendMode::Add, "add", "Add"},
}};
} // namespace

std::span<const BlendModeDescriptor> blendModes() {
    return modes;
}

std::optional<BlendMode> parseBlendMode(const std::optional<std::string>& name) {
    if (!name) return BlendMode::Normal;
    for (const auto& descriptor : modes)
        if (descriptor.name == *name) return descriptor.mode;
    return std::nullopt;
}

std::string_view blendModeName(BlendMode mode) {
    for (const auto& descriptor : modes)
        if (descriptor.mode == mode) return descriptor.name;
    return {};
}

} // namespace heisenberg
