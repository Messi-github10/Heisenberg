#pragma once

#include <Models/MediaManifest.hpp>
#include <Models/MediaResolver.hpp>

#include <Models/Timeline.hpp>
#include <Profiles/Profile.hpp>

#include <memory>
#include <string>
#include <vector>

namespace heisenberg {

class EditorViewModel {
public:
    EditorViewModel();
    ~EditorViewModel();

    EditorViewModel(const EditorViewModel&) = delete;
    EditorViewModel& operator=(const EditorViewModel&) = delete;

    void setHardwareDecode(bool enabled);
    void setProfile(Profile profile);

    bool importMedia(const std::string& path, std::string* error = nullptr);
    std::string placeClip(const std::string& mediaRef,
                          int64_t start,
                          int64_t in = 0,
                          int64_t out = -1,
                          std::string* error = nullptr);
    bool removeClip(const std::string& clipId, std::string* error = nullptr);
    bool moveClip(const std::string& clipId, int64_t start, std::string* error = nullptr);

    bool load(const std::string& projectDir, std::string* error = nullptr);
    bool save(const std::string& projectDir, std::string* error = nullptr) const;

    const MediaManifest& manifest() const;
    MediaResolver resolver() const;
    const Timeline& timeline() const;
    Timeline& timeline();
    std::vector<MediaAsset> assets() const;
    const std::string& projectDir() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace heisenberg
