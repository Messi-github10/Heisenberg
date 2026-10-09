#include "MediaManifest.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <initializer_list>
#include <limits>

namespace heisenberg {
namespace {

using json = nlohmann::ordered_json;

void setError(std::string* error, const std::string& message) {
    if (error) *error = message;
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

bool readOptionalInt64(const json& object,
                       const char* key,
                       int64_t min,
                       int64_t max,
                       int64_t& result,
                       const char* context,
                       std::string* error) {
    const json& value = jsonField(object, key);
    if (value.is_null() && !object.contains(key)) return true;
    return readInt64(object, key, min, max, result, context, error);
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

bool parseAssetId(const std::string& value, std::string& result, std::string* error) {
    if (value.empty()) {
        setError(error, "media id must not be empty");
        return false;
    }
    for (const char ch : value) {
        const bool ok = (ch >= 'a' && ch <= 'z') ||
                        (ch >= 'A' && ch <= 'Z') ||
                        (ch >= '0' && ch <= '9') ||
                        ch == '_' || ch == '-';
        if (!ok) {
            setError(error, "media id must match [A-Za-z0-9_-]+");
            return false;
        }
    }
    result = value;
    return true;
}

} // namespace

const char* toString(MediaType type) {
    switch (type) {
        case MediaType::Video: return "video";
        case MediaType::Audio: return "audio";
        case MediaType::Image: return "image";
    }
    return "video";
}

bool mediaTypeFromString(const std::string& value, MediaType& type) {
    if (value == "video") {
        type = MediaType::Video;
        return true;
    }
    if (value == "audio") {
        type = MediaType::Audio;
        return true;
    }
    if (value == "image") {
        type = MediaType::Image;
        return true;
    }
    return false;
}

bool MediaManifest::loadFromJson(const std::string& text, std::string* error) {
    json root;
    try {
        root = json::parse(text);
    } catch (const json::parse_error& parseError) {
        setError(error, std::string("Invalid media manifest JSON: ") + parseError.what());
        return false;
    }
    if (!root.is_object()) {
        setError(error, "media manifest JSON root must be an object");
        return false;
    }
    if (!expectKeys(root, {"version", "entries"}, "media", error)) return false;

    int64_t version = 1;
    if (!readInt64(root, "version", 1, 1, version, "media", error)) return false;

    const json& entriesValue = jsonField(root, "entries");
    if (!expectArray(entriesValue, "entries", error)) return false;

    std::vector<MediaManifestEntry> loaded;
    loaded.reserve(entriesValue.size());
    for (size_t index = 0; index < entriesValue.size(); ++index) {
        const json& entryValue = entriesValue[index];
        const std::string context = "entries[" + std::to_string(index) + "]";
        if (!expectObject(entryValue, context.c_str(), error) ||
            !expectKeys(entryValue,
                        {"id", "name", "type", "source", "durationFrames"},
                        context.c_str(), error)) {
            return false;
        }

        MediaManifestEntry entry;
        std::string id;
        std::string type;
        if (!readString(entryValue, "id", id, context.c_str(), error, true) ||
            !parseAssetId(id, entry.id, error) ||
            !readString(entryValue, "name", entry.name, context.c_str(), error, true) ||
            !readString(entryValue, "type", type, context.c_str(), error, true) ||
            !readOptionalInt64(entryValue, "durationFrames", 0,
                               std::numeric_limits<int64_t>::max(),
                               entry.durationFrames, context.c_str(), error)) {
            return false;
        }
        if (!mediaTypeFromString(type, entry.type)) {
            setError(error, context + " type '" + type + "' is unknown");
            return false;
        }
        if (std::find_if(loaded.begin(), loaded.end(),
                         [&](const MediaManifestEntry& existing) {
                             return existing.id == entry.id;
                         }) != loaded.end()) {
            setError(error, context + " id '" + entry.id + "' is duplicated");
            return false;
        }

        const json& sourceValue = jsonField(entryValue, "source");
        if (!expectObject(sourceValue, (context + ".source").c_str(), error) ||
            !expectKeys(sourceValue, {"kind", "path"},
                        (context + ".source").c_str(), error)) {
            return false;
        }
        std::string kind;
        if (!readString(sourceValue, "kind", kind, (context + ".source").c_str(),
                        error, true) ||
            !readString(sourceValue, "path", entry.source.path,
                        (context + ".source").c_str(), error, true)) {
            return false;
        }
        if (kind == "external") {
            entry.source.kind = MediaSource::Kind::External;
        } else if (kind == "project") {
            entry.source.kind = MediaSource::Kind::Project;
        } else {
            setError(error, context + ".source kind '" + kind + "' is unknown");
            return false;
        }
        if (entry.source.path.empty()) {
            setError(error, context + ".source path must not be empty");
            return false;
        }
        loaded.push_back(std::move(entry));
    }

    entries_ = std::move(loaded);
    return true;
}

std::string MediaManifest::toJson() const {
    json entries = json::array();
    for (const MediaManifestEntry& entry : entries_) {
        json source;
        source["kind"] = entry.source.kind == MediaSource::Kind::Project
            ? "project" : "external";
        source["path"] = entry.source.path;

        json object;
        object["id"] = entry.id;
        object["name"] = entry.name;
        object["type"] = toString(entry.type);
        object["source"] = std::move(source);
        object["durationFrames"] = entry.durationFrames;
        entries.push_back(std::move(object));
    }

    json root;
    root["version"] = 1;
    root["entries"] = std::move(entries);
    return root.dump(2);
}

bool MediaManifest::add(MediaManifestEntry entry, std::string* error) {
    if (!parseAssetId(entry.id, entry.id, error)) return false;
    if (find(entry.id)) {
        setError(error, "media id '" + entry.id + "' already exists");
        return false;
    }
    if (entry.source.path.empty()) {
        setError(error, "media source path must not be empty");
        return false;
    }
    entries_.push_back(std::move(entry));
    return true;
}

const MediaManifestEntry* MediaManifest::find(const std::string& id) const {
    for (const MediaManifestEntry& entry : entries_) {
        if (entry.id == id) return &entry;
    }
    return nullptr;
}

} // namespace heisenberg
