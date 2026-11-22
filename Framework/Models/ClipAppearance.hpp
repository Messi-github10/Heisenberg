#pragma once

#include <Models/Clip.hpp>
#include <Models/KeyframeSampling.hpp>
#include <array>
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

// Lightweight frame result. Edge rounding/softness remain clip constants.
struct SampledClipAppearance {
    double opacity = 1.0;
    Transform transform;
    Crop crop;
    bool operator==(const SampledClipAppearance&) const = default;
};

// Owns only validated numeric data, never Clip or JSON. Re-prepare after edits.
// Default construction samples the default Clip appearance.
class PreparedClipAppearance {
public:
    bool sample(int64_t clipLocalFrame, SampledClipAppearance& output,
                std::string* error = nullptr) const;

private:
    friend bool prepareClipAppearance(const Clip&, PreparedClipAppearance&, std::string*);
    SampledClipAppearance base_;
    std::vector<keyframes::Key<double>> opacity_;
    std::vector<keyframes::Key<std::array<double, 2>>> position_;
    std::vector<keyframes::Key<std::array<double, 2>>> scale_;
    std::vector<keyframes::Key<double>> rotation_;
    std::vector<keyframes::Key<std::array<double, 4>>> crop_;
};

// All keys (including off-frame keys), constant geometry and opacity are checked.
// Frames are clip-local and may be negative or outside the clip duration.
// Position, scale (width/height multipliers) and rotation replace constants.
// Missing tracks and empty keyframes use the constants. {} is malformed.
// All output arguments are changed only on success; payloads remain untouched.
bool prepareClipAppearance(const Clip& clip, PreparedClipAppearance& output,
                           std::string* error = nullptr);
bool validateClipAppearanceTracks(const Clip& clip, std::string* error = nullptr);
bool sampleClipAppearance(const Clip& clip, int64_t clipLocalFrame,
                          SampledClipAppearance& output, std::string* error = nullptr);
bool sampleClipAppearance(const Clip& clip, int64_t clipLocalFrame,
                          Clip& output, std::string* error = nullptr);

} // namespace heisenberg
