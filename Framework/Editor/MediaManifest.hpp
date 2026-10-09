#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace heisenberg {

enum class MediaType {
    Video,
    Audio,
    Image,
};

struct MediaSource {
    enum class Kind {
        External,
        Project,
    };

    Kind kind = Kind::External;
    std::string path;
};

struct MediaManifestEntry {
    std::string id;
    std::string name;
    MediaType type = MediaType::Video;
    MediaSource source;
    int64_t durationFrames = 0;
};

struct MediaAsset {
    std::string id;
    std::string name;
    MediaType type = MediaType::Video;
    std::string path;
    int64_t durationFrames = 0;
    bool missing = false;
};

class MediaManifest {
public:
    bool loadFromJson(const std::string& text, std::string* error = nullptr);
    std::string toJson() const;

    bool add(MediaManifestEntry entry, std::string* error = nullptr);
    const MediaManifestEntry* find(const std::string& id) const;
    const std::vector<MediaManifestEntry>& entries() const { return entries_; }

private:
    std::vector<MediaManifestEntry> entries_;
};

const char* toString(MediaType type);
bool mediaTypeFromString(const std::string& value, MediaType& type);

} // namespace heisenberg
