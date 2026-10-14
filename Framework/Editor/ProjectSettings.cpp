#include "ProjectSettings.hpp"
#include <Project/VideoProject.hpp>

namespace heisenberg {
ProjectSettings::ProjectSettings(VideoProject& project) : editor_(project.editor()) {}
ProjectSettings::ProjectSettings(EditorViewModel& editor) : editor_(editor) {}
TimelineSettings ProjectSettings::timelineSettings() const {
    const auto* timeline = editor_.activeTimeline();
    return timeline ? timeline->settings() : TimelineSettings{};
}
EditResult ProjectSettings::applyTimelineSettings(const TimelineSettings& settings) {
    return editor_.applyTimelineSettings(settings);
}
} // namespace heisenberg
