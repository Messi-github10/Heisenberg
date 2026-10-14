#pragma once

#include "Clip.hpp"
#include "TimelineSettings.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace heisenberg {

struct TimelineViewState {
    int64_t playheadFrame = 0;
    double zoomScale = 4.0;
    double scrollOffsetX = 0.0;
    bool operator==(const TimelineViewState&) const = default;
};

struct RgbaColor {
    double r = 0.0, g = 0.478, b = 1.0, a = 1.0;
    bool operator==(const RgbaColor&) const = default;
};

struct TimelineMarker {
    std::string id;
    std::string name;
    int64_t startFrame = 0;
    int64_t durationFrames = 0;
    RgbaColor color;
    std::string comment;
    std::string status = "open";
    int64_t endFrame() const;
    bool operator==(const TimelineMarker&) const = default;
};

struct Track {
    std::string id;
    ClipType type = ClipType::Video;
    std::optional<std::string> name;
    bool muted = false;
    bool hidden = false;
    bool syncLocked = true;
    std::vector<Clip> clips;
    double displayHeight = 44.0;
    int64_t endFrame() const;
    const Clip* clipAt(int64_t frame) const;
    const Clip* clipById(const std::string& id) const;
    Clip* clipById(const std::string& id);
    void sortClips();
    bool operator==(const Track&) const = default;
};

struct Timeline {
    std::string id;
    std::string name = "Timeline 1";
    int fps = 30, width = 1920, height = 1080;
    bool settingsConfigured = false;
    std::optional<std::string> folderId;
    std::vector<Track> tracks;
    std::vector<TimelineMarker> markers;
    TimelineSettings settings() const;
    bool applySettings(const TimelineSettings& settings, std::string* error = nullptr);
    int64_t totalFrames() const;
    const Clip* clipById(const std::string& id) const;
    Clip* clipById(const std::string& id);
    Track* trackById(const std::string& id);
    const Track* trackById(const std::string& id) const;
    Track* trackForClip(const std::string& clipId);
    const Track* trackForClip(const std::string& clipId) const;
    bool operator==(const Timeline&) const = default;
};

enum class CompatibilityLevel { Supported, Warning, Unsupported };
struct ProjectCompatibility {
    CompatibilityLevel level = CompatibilityLevel::Supported;
    std::vector<std::string> messages;
};

std::string makeUuid();
bool validateTimeline(const Timeline& timeline, std::string* error = nullptr);
ProjectCompatibility compatibilityFor(const Timeline& timeline);

} // namespace heisenberg
