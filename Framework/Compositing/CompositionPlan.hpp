#pragma once

#include <Models/TimelineSettings.hpp>
#include <Models/ColorSettings.hpp>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace heisenberg {
namespace grade { struct LutAsset; }

// Owned CPU-only snapshot. No project references, decoder, GPU objects or workers.
// A plan may share immutable validated LUT assets; no mutable library is retained.
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
    Clip visualSnapshot; // Owned complete clip appearance/effects/animation snapshot.
};

struct CompositionTrack {
    std::string id;
    ClipType type = ClipType::Video;
    bool hidden = false;
    bool muted = false;
    std::vector<CompositionClip> clips;
};

struct CompositionSample {
    size_t trackIndex = 0;
    std::string trackId;
    size_t clipIndex = 0;
    double sourceSeconds = 0.0;
    int64_t clipLocalFrame = 0;
    double speed = 1.0;
    // Base linear volume only; AudioMixer applies the clip's sample envelopes.
    double gain = 1.0;
    bool hidden = false;
    bool muted = false;
};

struct CompositionPlan {
    TimelineSettings settings;
    ColorSettings colorSettings;
    int64_t frameCount = 0;
    // Canonical owned tracks, in UI order (top first). Render bottom to top.
    std::vector<CompositionTrack> tracks;
    std::vector<std::string> warnings;
    // Captured once at plan build. Active renders retain immutable validated
    // CPU data even when the library cache/root or external files change.
    // Missing/corrupt assets stay diagnostic values, so preview may proceed
    // and strict compilation can report the same captured failure later.
    std::map<std::string, std::shared_ptr<const grade::LutAsset>> gradeLuts;
    std::map<std::string, std::string> gradeLutErrors;

    std::vector<CompositionSample> videoSamplesAt(int64_t frame) const;
    // Includes embedded audio on video tracks. Hidden affects pictures only.
    std::vector<CompositionSample> audioSamplesAt(int64_t frame) const;
    // Computed first-match conveniences, with no separate legacy storage.
    std::optional<CompositionSample> videoSampleAt(int64_t frame) const;
    std::optional<CompositionSample> audioSampleAt(int64_t frame) const;
    std::optional<CompositionSample> sampleAt(int64_t frame) const {
        return videoSampleAt(frame);
    }
};

} // namespace heisenberg
