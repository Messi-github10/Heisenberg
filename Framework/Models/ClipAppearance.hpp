#pragma once

#include <Models/Clip.hpp>
#include <optional>
#include <string>

namespace heisenberg {

// Omitted fields retain their exact stored values, including opaque automation.
struct ClipAppearancePatch {
    std::optional<double> centerX;
    std::optional<double> centerY;
    std::optional<double> width;
    std::optional<double> height;
    std::optional<double> rotation;
    std::optional<bool> flipHorizontal;
    std::optional<bool> flipVertical;
    std::optional<double> cropLeft;
    std::optional<double> cropTop;
    std::optional<double> cropRight;
    std::optional<double> cropBottom;
    std::optional<double> edgeRounding;
    std::optional<double> edgeSoftness;
    std::optional<double> opacity;
    std::optional<std::string> blendMode;

    bool empty() const;
};

// Checks constant 2D geometry only. Rotation X/Y and automation require separate
// capability checks and are deliberately outside this validation contract.
bool validateConstantGeometry(const Clip& clip, std::string* error = nullptr);

// Validate only supplied fields, using all merged sides when any crop changes.
bool validateClipAppearancePatch(const Clip& clip, const ClipAppearancePatch& patch,
                                 std::string* error = nullptr);

} // namespace heisenberg
