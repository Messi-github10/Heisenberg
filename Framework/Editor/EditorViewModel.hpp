#pragma once

#include <Models/MediaManifest.hpp>
#include <Models/MediaResolver.hpp>
#include <Models/Timeline.hpp>
#include <Profiles/Profile.hpp>

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
    bool changed() const { return status == EditStatus::Changed; }
};

enum class TrimEdge { Left, Right };

class EditorViewModel {
public:
    EditorViewModel();
    ~EditorViewModel();
    EditorViewModel(const EditorViewModel&) = delete;
    EditorViewModel& operator=(const EditorViewModel&) = delete;

    void setHardwareDecode(bool enabled);
    void setProfile(Profile profile);
    bool importMedia(const std::string& path, std::string* error = nullptr);

    EditResult placeClip(const std::string& mediaRef, int64_t startFrame,
                         int64_t durationFrames = -1, int64_t trimStartFrame = 0);
    EditResult moveClip(const std::string& clipId, int64_t startFrame);
    EditResult splitClip(const std::string& clipId, int64_t atFrame);
    EditResult trimClip(const std::string& clipId, TrimEdge edge, int64_t deltaFrames);
    EditResult removeClips(const std::vector<std::string>& clipIds);
    bool undo();
    bool redo();

    bool loadPackage(const std::string& packageDir, std::string* error = nullptr);
    bool savePackage(const std::string& packageDir, std::string* error = nullptr);

    const ProjectFile& project() const;
    const MediaManifest& manifest() const;
    MediaResolver resolver() const;
    const Timeline* activeTimeline() const;
    Timeline* activeTimeline();
    const Track* activeTrack() const;
    Track* activeTrack();
    std::vector<MediaAsset> assets() const;
    const std::string& projectDir() const;
    ProjectCompatibility compatibility() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace heisenberg
