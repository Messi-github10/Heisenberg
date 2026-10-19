#pragma once

#include <Models/MediaManifest.hpp>
#include <Models/MediaResolver.hpp>
#include "EditorViewModelTimelines.hpp"
#include <Models/TimelineSettings.hpp>
#include <functional>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace heisenberg {

enum class EditStatus { Changed, NoOp, Unsupported, Invalid };

struct EditResult {
    EditStatus status = EditStatus::NoOp;
    std::vector<std::string> affectedIds;
    std::string message;

    bool changed() const {
        return status == EditStatus::Changed;
    }
};

enum class TrimEdge { Left, Right };

class EditorViewModel {
public:
    EditorViewModel();
    ~EditorViewModel();
    EditorViewModel(const EditorViewModel&) = delete;
    EditorViewModel& operator=(const EditorViewModel&) = delete;

    void setHardwareDecode(bool enabled);
    void setProfile(TimelineSettings settings);
    EditResult applyTimelineSettings(const TimelineSettings& settings);
    const EditorViewModelTimelines& timelines() const;
    uint64_t revision() const;
    bool importMedia(const std::string& path, std::string* error = nullptr);

    EditResult placeClip(const std::string& mediaRef, int64_t startFrame,
                         int64_t durationFrames = -1, int64_t trimStartFrame = 0,
                         const std::string& targetTrackId = {});
    EditResult setClipAudio(const std::string& clipId, double volume,
                            int64_t fadeInFrames, int64_t fadeOutFrames);
    EditResult setTrackMuted(const std::string& trackId, bool muted);
    EditResult setColorSettings(const ColorSettings& settings);
    EditResult setMediaInputColorSpace(const std::string& mediaRef, const std::string& space);
    EditResult setClipEffects(const std::string& clipId, std::optional<JsonValue> effects);
    EditResult moveClip(const std::string& clipId, int64_t startFrame);
    EditResult splitClip(const std::string& clipId, int64_t atFrame);
    EditResult trimClip(const std::string& clipId, TrimEdge edge, int64_t deltaFrames);
    EditResult removeClips(const std::vector<std::string>& clipIds);

    bool undo();
    bool redo();

    bool replaceDocument(ProjectFile project, MediaManifest manifest,
                         std::string packageDir = {}, std::string* error = nullptr);

    const ProjectFile& project() const;
    const MediaManifest& manifest() const;
    MediaResolver resolver() const;
    const Timeline* activeTimeline() const;
    const Track* activeTrack() const;
    std::vector<MediaAsset> assets() const;
    const std::string& projectDir() const;
    ProjectCompatibility compatibility() const;

private:
    friend class VideoProject;
    void setProjectDir(std::string root);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace heisenberg
