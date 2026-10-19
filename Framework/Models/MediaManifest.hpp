#pragma once

#include "Clip.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace heisenberg {

struct MediaSource {
    enum class Kind { External, Project };
    Kind kind = Kind::External;
    std::string path;
    bool operator==(const MediaSource&) const = default;
};

struct MediaFolder {
    std::string id;
    std::string name;
    std::optional<std::string> parentFolderId;
    bool operator==(const MediaFolder&) const = default;
};

struct MediaManifestEntry {
    std::string id;
    std::string name;
    ClipType type = ClipType::Video;
    MediaSource source;
    double duration = 0.0;
    std::optional<JsonValue> generationInput;
    std::optional<int> sourceWidth;
    std::optional<int> sourceHeight;
    std::optional<double> sourceFPS;
    std::optional<bool> hasAudio;
    std::optional<std::string> folderId;
    std::optional<std::string> cachedRemoteURL;
    std::optional<double> cachedRemoteURLExpiresAt;
    std::optional<std::string> generationStatus;
    std::optional<JsonValue> importInput;
    bool operator==(const MediaManifestEntry&) const = default;
};

struct MediaAsset {
    std::string id;
    std::string name;
    ClipType type = ClipType::Video;
    std::string path;
    double duration = 0.0;
    bool missing = false;
};

class MediaManifest {
public:
    bool loadFromJson(const std::string& text, std::string* error = nullptr);
    std::string toJson() const;
    bool validate(std::string* error = nullptr) const;
    bool add(MediaManifestEntry entry, std::string* error = nullptr);
    const MediaManifestEntry* find(const std::string& id) const;

    const std::vector<MediaManifestEntry>& entries() const {
        return entries_;
    }

    const std::vector<MediaFolder>& folders() const {
        return folders_;
    }

    std::vector<MediaFolder>& folders() {
        return folders_;
    }
    
    bool operator==(const MediaManifest&) const = default;

private:
    int version_ = 2;
    std::vector<MediaManifestEntry> entries_;
    std::vector<MediaFolder> folders_;
};

} // namespace heisenberg
