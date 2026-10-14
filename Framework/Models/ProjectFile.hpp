#pragma once

#include "Timeline.hpp"
#include <unordered_map>

namespace heisenberg {

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

bool validateProject(const ProjectFile& project, std::string* error = nullptr);

} // namespace heisenberg
