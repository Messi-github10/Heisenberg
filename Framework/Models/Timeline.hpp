#pragma once

#include "Clip.hpp"
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
    int64_t totalFrames() const;
    const Clip* clipById(const std::string& id) const;
    Clip* clipById(const std::string& id);
    Track* trackById(const std::string& id);
    const Track* trackById(const std::string& id) const;
    bool operator==(const Timeline&) const = default;
};

struct SpeakerRegistryEntry {
    int id = 0;
    std::string name;
    std::vector<double> color;
    std::vector<float> centroid;
    bool operator==(const SpeakerRegistryEntry&) const = default;
};

struct MulticamSource {
    std::string id;
    std::string name;
    JsonValue members = JsonValue::array();
    std::string masterMemberId;
    bool operator==(const MulticamSource&) const = default;
};

struct ProjectFile {
    std::vector<Timeline> timelines;
    std::optional<std::string> activeTimelineId;
    std::optional<std::vector<std::string>> openTimelineIds;
    std::optional<std::unordered_map<std::string, TimelineViewState>> viewStates;
    std::optional<std::vector<SpeakerRegistryEntry>> speakers;
    std::optional<std::vector<MulticamSource>> multicamGroups;
    Timeline* activeTimeline();
    const Timeline* activeTimeline() const;
    bool operator==(const ProjectFile&) const = default;
};

enum class CompatibilityLevel { Supported, Warning, Unsupported };
struct ProjectCompatibility {
    CompatibilityLevel level = CompatibilityLevel::Supported;
    std::vector<std::string> messages;
};

class ProjectCodec {
public:
    static bool decode(const std::string& text, ProjectFile& result,
                       std::string* error = nullptr);
    static std::string encode(const ProjectFile& project);
};

std::string makeUuid();
bool validateProject(const ProjectFile& project, std::string* error = nullptr);
bool validateTimeline(const Timeline& timeline, std::string* error = nullptr);
ProjectCompatibility compatibilityFor(const Timeline& timeline);

} // namespace heisenberg
