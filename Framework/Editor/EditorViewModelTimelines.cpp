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
    if (!timeline || compatibilityFor(*timeline).level == CompatibilityLevel::Unsupported) return nullptr;
    for (const auto& track : timeline->tracks) if (track.type == ClipType::Video) return &track;
    return timeline->tracks.empty() ? nullptr : &timeline->tracks.front();
}
Track* EditorViewModelTimelines::activeTrack() {
    return const_cast<Track*>(static_cast<const EditorViewModelTimelines&>(*this).activeTrack());
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
