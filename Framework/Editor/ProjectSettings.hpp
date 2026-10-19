#pragma once

#include "EditorViewModel.hpp"

namespace heisenberg {

class VideoProject;

class ProjectSettings {
public:
    explicit ProjectSettings(VideoProject& project);
    explicit ProjectSettings(EditorViewModel& editor);
    TimelineSettings timelineSettings() const;
    EditResult applyTimelineSettings(const TimelineSettings& settings);
private:
    EditorViewModel& editor_;
};

} // namespace heisenberg
