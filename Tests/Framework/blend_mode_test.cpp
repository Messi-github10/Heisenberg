#include <Models/BlendMode.hpp>
#include <gtest/gtest.h>

#include <array>
#include <set>

namespace {
using namespace heisenberg;

TEST(BlendModeTest, DescriptorsHaveStableOrderNamesAndRoundTrip) {
    constexpr std::array<std::string_view, 17> names{
        "normal", "darken", "multiply", "colorBurn", "lighten", "screen",
        "colorDodge", "overlay", "softLight", "hardLight", "difference",
        "exclusion", "hue", "saturation", "color", "luminosity", "add"
    };
    constexpr std::array<BlendMode, 17> expectedModes{
        BlendMode::Normal, BlendMode::Darken, BlendMode::Multiply, BlendMode::ColorBurn,
        BlendMode::Lighten, BlendMode::Screen, BlendMode::ColorDodge, BlendMode::Overlay,
        BlendMode::SoftLight, BlendMode::HardLight, BlendMode::Difference, BlendMode::Exclusion,
        BlendMode::Hue, BlendMode::Saturation, BlendMode::Color, BlendMode::Luminosity, BlendMode::Add
    };
    const auto modes = blendModes();
    ASSERT_EQ(modes.size(), names.size());
    EXPECT_EQ(static_cast<int>(BlendMode::Normal), 0);
    std::set<std::string_view> uniqueNames;
    std::set<std::string_view> uniqueDisplayNames;
    for (std::size_t i = 0; i < names.size(); ++i) {
        SCOPED_TRACE(names[i]);
        EXPECT_EQ(modes[i].mode, expectedModes[i]);
        EXPECT_EQ(static_cast<int>(modes[i].mode), static_cast<int>(i));
        EXPECT_EQ(modes[i].name, names[i]);
        EXPECT_FALSE(modes[i].displayName.empty());
        EXPECT_TRUE(uniqueNames.insert(modes[i].name).second);
        EXPECT_TRUE(uniqueDisplayNames.insert(modes[i].displayName).second);
        EXPECT_EQ(blendModeName(modes[i].mode), names[i]);
        const auto parsed = parseBlendMode(std::string(names[i]));
        ASSERT_TRUE(parsed);
        EXPECT_EQ(*parsed, modes[i].mode);
    }
    EXPECT_EQ(blendModes().data(), modes.data());
}

TEST(BlendModeTest, AbsenceMeansNormalButExplicitUnknownNamesAreRejected) {
    EXPECT_EQ(parseBlendMode(std::nullopt), BlendMode::Normal);
    for (const auto* name : {"", "Normal", "MULTIPLY", "colorburn", "ColorBurn", "softlight",
                              "hard-light", "color_dodge", " normal", "normal ", "plus",
                              "addition", "source-over", "futureBlend"}) {
        SCOPED_TRACE(name);
        EXPECT_FALSE(parseBlendMode(std::string(name)));
    }
    EXPECT_FALSE(parseBlendMode(std::string("normal\0suffix", 13)));
}

TEST(BlendModeTest, InvalidEnumValuesHaveNoCanonicalName) {
    EXPECT_TRUE(blendModeName(static_cast<BlendMode>(-1)).empty());
    EXPECT_TRUE(blendModeName(static_cast<BlendMode>(17)).empty());
    EXPECT_TRUE(blendModeName(static_cast<BlendMode>(999)).empty());
}
} // namespace
