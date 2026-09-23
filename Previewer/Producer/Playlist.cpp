#include "Playlist.hpp"

#include "Producer.hpp"

#include <nlohmann/json.hpp>
#include <Utiles/Logger.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>

namespace heisenberg {
namespace {

using json = nlohmann::ordered_json;

void setError(std::string* error, const std::string& message) {
    if (error) *error = message;
}

std::string utf8FromPath(const std::filesystem::path& path) {
    const auto utf8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(utf8.c_str()), utf8.size());
}

const json& jsonField(const json& object, const char* key) {
    static const json missing;
    const auto it = object.find(key);
    return it == object.end() ? missing : *it;
}

bool expectObject(const json& value, const char* name, std::string* error) {
    if (value.is_object()) return true;
    setError(error, std::string("JSON field '") + name + "' must be an object");
    return false;
}

bool expectArray(const json& value, const char* name, std::string* error) {
    if (value.is_array()) return true;
    setError(error, std::string("JSON field '") + name + "' must be an array");
    return false;
}

bool expectKeys(const json& object,
                std::initializer_list<const char*> allowed,
                const char* context,
                std::string* error) {
    for (auto it = object.begin(); it != object.end(); ++it) {
        bool found = false;
        for (const char* key : allowed) {
            if (it.key() == key) {
                found = true;
                break;
            }
        }
        if (!found) {
            setError(error, std::string(context) + " contains unknown field '" +
                                it.key() + "'");
            return false;
        }
    }
    return true;
}

bool readInt64(const json& object,
               const char* key,
               int64_t min,
               int64_t max,
               int64_t& result,
               const char* context,
               std::string* error) {
    const json& value = jsonField(object, key);
    if (value.is_null() && !object.contains(key)) {
        setError(error, std::string(context) + " is missing '" + key + "'");
        return false;
    }
    if (!value.is_number_integer()) {
        setError(error, std::string(context) + " field '" + key +
                            "' must be an integer");
        return false;
    }

    int64_t number = 0;
    if (value.is_number_unsigned()) {
        const auto unsignedNumber = value.get<uint64_t>();
        if (unsignedNumber > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            setError(error, std::string(context) + " field '" + key +
                                "' is out of range");
            return false;
        }
        number = static_cast<int64_t>(unsignedNumber);
    } else {
        number = value.get<int64_t>();
    }

    if (number < min || number > max) {
        setError(error, std::string(context) + " field '" + key +
                            "' is out of range");
        return false;
    }
    result = number;
    return true;
}

bool readString(const json& object,
                const char* key,
                std::string& result,
                const char* context,
                std::string* error,
                bool required) {
    const json& value = jsonField(object, key);
    if (value.is_null() && !object.contains(key)) {
        if (!required) {
            result.clear();
            return true;
        }
        setError(error, std::string(context) + " is missing '" + key + "'");
        return false;
    }
    if (!value.is_string()) {
        setError(error, std::string(context) + " field '" + key +
                            "' must be a string");
        return false;
    }
    result = value.get<std::string>();
    return true;
}

bool parseClipId(const std::string& value, std::string& result, std::string* error) {
    if (value.empty()) {
        result.clear();
        return true;
    }
    for (const char ch : value) {
        const bool ok = (ch >= 'a' && ch <= 'z') ||
                        (ch >= '0' && ch <= '9') ||
                        ch == '_';
        if (!ok) {
            setError(error, "clip.id must match [a-z0-9_]+");
            return false;
        }
    }
    result = value;
    return true;
}

std::string resolveResource(const std::string& resource, const std::string& baseDir) {
    const auto path = std::filesystem::u8path(resource);
    if (path.is_absolute() || baseDir.empty()) return utf8FromPath(path);
    return utf8FromPath(std::filesystem::u8path(baseDir) / path);
}

} // namespace

struct Playlist::Impl {
    Profile profile = Profile::hd1080p24();
    std::string resource = "<playlist>";
    bool hardwareDecode = false;
    std::vector<PlaylistClip> clips;
    std::unordered_map<std::string, std::unique_ptr<Producer>> producers;
    int64_t length = 0;
    int64_t position = 0;

    void reset() {
        clips.clear();
        producers.clear();
        length = 0;
        position = 0;
        resource = "<playlist>";
        profile = Profile::hd1080p24();
    }

    Producer* producerFor(const std::string& path) {
        const auto found = producers.find(path);
        return found == producers.end() ? nullptr : found->second.get();
    }

    const PlaylistClip* clipAt(int64_t timelinePosition) const {
        for (const PlaylistClip& clip : clips) {
            if (timelinePosition >= clip.start &&
                timelinePosition < clip.start + clip.duration()) {
                return &clip;
            }
        }
        return nullptr;
    }
};

Playlist::Playlist()
    : impl_(std::make_unique<Impl>()) {}

Playlist::~Playlist() = default;

void Playlist::setHardwareDecode(bool enabled) {
    impl_->hardwareDecode = enabled;
}

bool Playlist::loadFromJsonFile(const std::string& path, std::string* error) {
    std::ifstream file(std::filesystem::u8path(path), std::ios::binary);
    if (!file) {
        setError(error, "Failed to open Playlist JSON '" + path + "'");
        return false;
    }
    std::string text((std::istreambuf_iterator<char>(file)),
                     std::istreambuf_iterator<char>());
    const auto parent = std::filesystem::u8path(path).parent_path();
    return loadFromJson(text, error, utf8FromPath(parent));
}

bool Playlist::loadFromJson(const std::string& text,
                            std::string* error,
                            const std::string& baseDir) {
    json root;
    try {
        root = json::parse(text);
    } catch (const json::parse_error& parseError) {
        setError(error, std::string("Invalid Playlist JSON: ") + parseError.what());
        return false;
    }
    if (!root.is_object()) {
        setError(error, "Playlist JSON root must be an object");
        return false;
    }
    if (!expectKeys(root, {"version", "profile", "clips"}, "playlist", error)) {
        return false;
    }

    int64_t version = 0;
    if (!readInt64(root, "version", 1, 1, version, "playlist", error)) {
        return false;
    }

    const json& profileValue = jsonField(root, "profile");
    if (!expectObject(profileValue, "profile", error)) return false;
    Profile profile;
    if (profileValue.contains("id") && !profileValue.contains("video")) {
        if (!expectKeys(profileValue, {"id"}, "profile", error)) return false;
        std::string profileId;
        if (!readString(profileValue, "id", profileId, "profile", error, true)) {
            return false;
        }
        const Profile* preset = Profile::findPreset(profileId);
        if (!preset) {
            setError(error, "Unknown profile id '" + profileId + "'");
            return false;
        }
        profile = *preset;
    } else if (!profile.loadFromJson(profileValue.dump(), error)) {
        return false;
    }

    const json& clipsValue = jsonField(root, "clips");
    if (!expectArray(clipsValue, "clips", error)) return false;
    if (clipsValue.empty()) {
        setError(error, "playlist.clips must not be empty");
        return false;
    }

    std::vector<PlaylistClip> clips;
    clips.reserve(clipsValue.size());
    int64_t timeline = 0;
    for (size_t index = 0; index < clipsValue.size(); ++index) {
        const json& clipValue = clipsValue[index];
        const std::string context = "clips[" + std::to_string(index) + "]";
        if (!expectObject(clipValue, context.c_str(), error) ||
            !expectKeys(clipValue, {"id", "resource", "in", "out"},
                        context.c_str(), error)) {
            return false;
        }

        PlaylistClip clip;
        std::string id;
        std::string resource;
        if (!readString(clipValue, "id", id, context.c_str(), error, false) ||
            !parseClipId(id, clip.id, error) ||
            !readString(clipValue, "resource", resource, context.c_str(), error, true) ||
            !readInt64(clipValue, "in", 0, std::numeric_limits<int64_t>::max(),
                       clip.in, context.c_str(), error) ||
            !readInt64(clipValue, "out", 0, std::numeric_limits<int64_t>::max(),
                       clip.out, context.c_str(), error)) {
            return false;
        }
        if (resource.empty()) {
            setError(error, context + " resource must not be empty");
            return false;
        }
        if (clip.out < clip.in) {
            setError(error, context + " out must be >= in");
            return false;
        }
        clip.resource = resolveResource(resource, baseDir);
        clip.start = timeline;
        timeline += clip.duration();
        clips.push_back(std::move(clip));
    }

    std::unordered_map<std::string, std::unique_ptr<Producer>> producers;
    for (PlaylistClip& clip : clips) {
        Producer* producer = nullptr;
        const auto found = producers.find(clip.resource);
        if (found == producers.end()) {
            auto created = std::make_unique<Producer>(profile);
            created->setHardwareDecode(impl_->hardwareDecode);
            std::string openError;
            if (!created->open(clip.resource, &openError)) {
                setError(error, "Failed to open clip resource '" + clip.resource +
                                    "': " + openError);
                return false;
            }
            producer = created.get();
            producers.emplace(clip.resource, std::move(created));
        } else {
            producer = found->second.get();
        }

        if (clip.out >= producer->length()) {
            setError(error, "clip out " + std::to_string(clip.out) +
                                " is past the end of '" + clip.resource + "'");
            return false;
        }
    }

    impl_->profile = std::move(profile);
    impl_->clips = std::move(clips);
    impl_->producers = std::move(producers);
    impl_->length = timeline;
    impl_->position = 0;
    impl_->resource = "<playlist>";
    LOG_INFO("Playlist: loaded {} clips, {} frames",
             impl_->clips.size(), impl_->length);
    return true;
}

std::string Playlist::toJson() const {
    json clips = json::array();
    for (const PlaylistClip& clip : impl_->clips) {
        json object;
        if (!clip.id.empty()) object["id"] = clip.id;
        object["resource"] = clip.resource;
        object["in"] = clip.in;
        object["out"] = clip.out;
        clips.push_back(std::move(object));
    }

    json root;
    root["version"] = 1;
    root["profile"] = json::parse(impl_->profile.toJson());
    root["clips"] = std::move(clips);
    return root.dump(2);
}

const std::vector<PlaylistClip>& Playlist::clips() const {
    return impl_->clips;
}

const Profile& Playlist::profile() const {
    return impl_->profile;
}

const std::string& Playlist::resource() const {
    return impl_->resource;
}

int64_t Playlist::in() const {
    return 0;
}

int64_t Playlist::out() const {
    return impl_->length > 0 ? impl_->length - 1 : 0;
}

int64_t Playlist::length() const {
    return impl_->length;
}

int64_t Playlist::position() const {
    return impl_->position;
}

bool Playlist::seekable() const {
    if (impl_->producers.empty()) return false;
    for (const auto& [path, producer] : impl_->producers) {
        if (!producer || !producer->seekable()) return false;
    }
    return true;
}

bool Playlist::seek(int64_t position) {
    if (impl_->length <= 0) return false;
    impl_->position = std::clamp(position, int64_t{0}, impl_->length - 1);
    return true;
}

ProducerFrame Playlist::getFrame(int64_t position) {
    ProducerFrame frame;
    if (impl_->length <= 0) {
        frame.eof = true;
        return frame;
    }

    if (position < 0 || position >= impl_->length) {
        frame.eof = true;
        frame.position = std::clamp(position, int64_t{0}, impl_->length - 1);
        impl_->position = frame.position;
        return frame;
    }

    const PlaylistClip* clip = impl_->clipAt(position);
    Producer* producer = clip ? impl_->producerFor(clip->resource) : nullptr;
    if (!clip || !producer) {
        frame.eof = true;
        frame.position = position;
        impl_->position = position;
        return frame;
    }

    const int64_t sourcePosition = clip->in + (position - clip->start);
    frame = producer->getFrame(sourcePosition);
    frame.position = position;
    impl_->position = position;
    return frame;
}

} // namespace heisenberg
