#include "ProjectFile.hpp"

#include <unordered_set>

namespace heisenberg {
namespace {
void setError(std::string* error, const std::string& message) { if (error) *error = message; }
}

Timeline* ProjectFile::activeTimeline() {
    if (timelines.empty()) return nullptr;
    if (activeTimelineId) for (Timeline& timeline : timelines) if (timeline.id == *activeTimelineId) return &timeline;
    return &timelines.front();
}

const Timeline* ProjectFile::activeTimeline() const {
    if (timelines.empty()) return nullptr;
    if (activeTimelineId) for (const Timeline& timeline : timelines) if (timeline.id == *activeTimelineId) return &timeline;
    return &timelines.front();
}

bool validateProject(const ProjectFile& project, std::string* error) {
    if (project.timelines.empty()) { setError(error, "project has no timelines"); return false; }
    std::unordered_set<std::string> ids;
    for (const Timeline& timeline : project.timelines) {
        if (!ids.insert(timeline.id).second) { setError(error, "timeline ids must be unique"); return false; }
        if (!validateTimeline(timeline, error)) return false;
    }
    if (project.activeTimelineId && !ids.count(*project.activeTimelineId)) {
        setError(error, "activeTimelineId does not name a timeline"); return false;
    }
    std::unordered_set<std::string> openIds;
    if (project.openTimelineIds) for (const std::string& id : *project.openTimelineIds) {
        if (!ids.count(id) || !openIds.insert(id).second) {
            setError(error, "openTimelineIds contains an unknown or duplicated timeline"); return false;
        }
    }
    return true;
}

} // namespace heisenberg
