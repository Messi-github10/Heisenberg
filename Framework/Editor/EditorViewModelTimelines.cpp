#include "EditorViewModelTimelines.hpp"

#include <algorithm>

namespace heisenberg {

EditorViewModelTimelines::EditorViewModelTimelines() {
    Timeline timeline;
    timeline.id = makeUuid();
    project_.timelines.push_back(timeline);
    project_.activeTimelineId = timeline.id;
    project_.openTimelineIds = std::vector<std::string>{timeline.id};
    project_.viewStates = std::unordered_map<std::string, TimelineViewState>{};
}
const Track* EditorViewModelTimelines::activeTrack() const {
    const auto* timeline = activeTimeline();
    return timeline && timeline->tracks.size() == 1 ? &timeline->tracks.front() : nullptr;
}
Track* EditorViewModelTimelines::activeTrack() {
    auto* timeline = activeTimeline();
    return timeline && timeline->tracks.size() == 1 ? &timeline->tracks.front() : nullptr;
}
void EditorViewModelTimelines::replace(ProjectFile project) {
    project_ = std::move(project);
    undo_.clear();
    redo_.clear();
}
bool EditorViewModelTimelines::undo() {
    if (undo_.empty()) return false;
    auto found = std::find_if(project_.timelines.begin(), project_.timelines.end(),
                             [&](const auto& timeline) { return timeline.id == undo_.back().timelineId; });
    if (found == project_.timelines.end()) return false;
    auto entry = std::move(undo_.back());
    undo_.pop_back();
    *found = entry.before;
    project_.activeTimelineId = entry.timelineId;
    redo_.push_back(std::move(entry));
    return true;
}
bool EditorViewModelTimelines::redo() {
    if (redo_.empty()) return false;
    auto found = std::find_if(project_.timelines.begin(), project_.timelines.end(),
                             [&](const auto& timeline) { return timeline.id == redo_.back().timelineId; });
    if (found == project_.timelines.end()) return false;
    auto entry = std::move(redo_.back());
    redo_.pop_back();
    *found = entry.after;
    project_.activeTimelineId = entry.timelineId;
    undo_.push_back(std::move(entry));
    return true;
}

} // namespace heisenberg
