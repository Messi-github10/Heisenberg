#pragma once

#include <Models/TimelineSettings.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace heisenberg {

// A value-only snapshot. No project references, decoder, GPU objects or workers.
struct CompositionClip {
    std::string id;
    std::string mediaRef;
    std::string sourcePath;
    int64_t startFrame = 0;
    int64_t durationFrames = 0;
    int64_t trimStartFrame = 0;
    int64_t trimEndFrame = 0;
    double speed = 1.0;
    double volume = 1.0;
    int64_t fadeInFrames = 0;
    int64_t fadeOutFrames = 0;
};

struct CompositionSample {
    size_t clipIndex = 0;
    double sourceSeconds = 0.0;
    double speed = 1.0;
    double gain = 1.0;
    bool hidden = false;
    bool muted = false;
};

struct CompositionPlan {
    TimelineSettings settings;
    int64_t frameCount = 0;
    bool hidden = false;
    bool muted = false;
    std::vector<CompositionClip> clips;
    std::vector<std::string> warnings;

    std::optional<CompositionSample> sampleAt(int64_t frame) const;
};

} // namespace heisenberg
