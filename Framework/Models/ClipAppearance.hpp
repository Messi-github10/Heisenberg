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
    bool operator==(const ClipAppearancePatch&) const = default;
};

enum class AppearanceProperty { Opacity, Position, Scale, Rotation, Crop };
enum class AppearanceEditAction { Values, ToggleKeyframe, SetInterpolation };

struct ClipAppearanceEdit {
    // No property permits only constant extras: flips, edge styling and blend mode.
    std::optional<AppearanceProperty> property;
    int64_t localFrame = 0;
    AppearanceEditAction action = AppearanceEditAction::Values;
    ClipAppearancePatch patch;
    Interpolation interpolation = Interpolation::Linear;
};

struct ClipAppearancePropertyState {
    ClipAppearancePatch sampled;
    std::vector<int64_t> keyframes;
    bool exactKey = false;
    bool animated = false;
    // Exact key's outgoing interpolation, otherwise the preceding key's, or Linear.
    Interpolation interpolation = Interpolation::Linear;
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

// Cached, target-only inspection. Owns numeric data, never Clip or opaque JSON.
class PreparedAppearanceProperty {
public:
    bool sample(int64_t localFrame, ClipAppearancePropertyState& output,
                std::string* error = nullptr) const;
    const std::vector<int64_t>& keyframes() const { return frames_; }
private:
    friend bool prepareAppearanceProperty(const Clip&, AppearanceProperty,
                                          PreparedAppearanceProperty&, std::string*);
    AppearanceProperty property_ = AppearanceProperty::Opacity;
    PreparedClipAppearance prepared_;
    std::vector<int64_t> frames_;
    std::vector<Interpolation> interpolations_;
};

bool prepareAppearanceProperty(const Clip& clip, AppearanceProperty property,
                               PreparedAppearanceProperty& output, std::string* error = nullptr);
bool inspectClipAppearanceProperty(const Clip& clip, AppearanceProperty property, int64_t localFrame,
                                   ClipAppearancePropertyState& output, std::string* error = nullptr);
// Atomic target-only edit. Key writes require [0,duration); constants and inspection accept any frame.
// Retains unknown track/key/value members and all untouched appearance properties.
bool applyClipAppearanceEdit(const Clip& clip, const ClipAppearanceEdit& edit,
                             Clip& output, std::string* error = nullptr);

} // namespace heisenberg
