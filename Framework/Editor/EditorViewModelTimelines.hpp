#pragma once

#include <Models/ProjectFile.hpp>

namespace heisenberg {

class EditorViewModel;

// Timeline values and their edit history form a member of the one editor.
class EditorViewModelTimelines {
public:
    EditorViewModelTimelines();
    const ProjectFile& project() const { return project_; }
    const Timeline* activeTimeline() const { return project_.activeTimeline(); }
    Timeline* activeTimeline() { return project_.activeTimeline(); }
    const Track* activeTrack() const;
    Track* activeTrack();
    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }

private:
    friend class EditorViewModel;
    struct UndoEntry {
        std::string timelineId; Timeline before; Timeline after; std::string name;
        std::optional<ColorSettings> colorBefore;
        std::optional<ColorSettings> colorAfter;
    };
    void replace(ProjectFile project);
    bool undo();
    bool redo();
    ProjectFile project_;
    std::vector<UndoEntry> undo_;
    std::vector<UndoEntry> redo_;
};

} // namespace heisenberg
