#include "MediaManifest.hpp"
#include <cmath>
#include <unordered_set>

namespace heisenberg {
namespace {
using json = nlohmann::ordered_json;

void setError(std::string* error, const std::string& message) {
    if (error) *error = message;
}

template <typename T>
std::optional<T> optionalValue(const json& object, const char* key) {
    const auto found = object.find(key);
    if (found == object.end() || found->is_null()) return std::nullopt;
    return found->get<T>();
}

template <typename T>
void putOptional(json& object, const char* key, const std::optional<T>& value) {
    if (value) object[key] = *value;
}

json sourceJson(const MediaSource& source) {
    if (source.kind == MediaSource::Kind::Project)
        return {{"project", {{"relativePath", source.path}}}};
    return {{"external", {{"absolutePath", source.path}}}};
}

MediaSource parseSource(const json& value) {
    if (value.contains("external"))
        return {MediaSource::Kind::External, value.at("external").at("absolutePath").get<std::string>()};
    if (value.contains("project"))
        return {MediaSource::Kind::Project, value.at("project").at("relativePath").get<std::string>()};
    throw std::runtime_error("media source must be external or project");
}
}

bool MediaManifest::loadFromJson(const std::string& text, std::string* error) {
    try {
        const json root = json::parse(text);
        if (!root.is_object()) throw std::runtime_error("media root must be an object");
        const int version = root.value("version", 1);
        if (version != 2) throw std::runtime_error("media manifest version must be 2");
        std::vector<MediaManifestEntry> entries;
        std::unordered_set<std::string> ids;
        for (const json& value : root.value("entries", json::array())) {
            MediaManifestEntry entry;
            entry.id = value.at("id").get<std::string>();
            entry.name = value.at("name").get<std::string>();
            if (!clipTypeFromString(value.at("type").get<std::string>(), entry.type))
                throw std::runtime_error("unknown media type");
            entry.source = parseSource(value.at("source"));
            entry.duration = value.at("duration").get<double>();
            if (entry.id.empty() || !ids.insert(entry.id).second || entry.source.path.empty() ||
                !std::isfinite(entry.duration) || entry.duration < 0)
                throw std::runtime_error("invalid or duplicated media entry");
            entry.generationInput = optionalValue<JsonValue>(value, "generationInput");
            entry.sourceWidth = optionalValue<int>(value, "sourceWidth");
            entry.sourceHeight = optionalValue<int>(value, "sourceHeight");
            entry.sourceFPS = optionalValue<double>(value, "sourceFPS");
            entry.hasAudio = optionalValue<bool>(value, "hasAudio");
            entry.folderId = optionalValue<std::string>(value, "folderId");
            entry.cachedRemoteURL = optionalValue<std::string>(value, "cachedRemoteURL");
            entry.cachedRemoteURLExpiresAt = optionalValue<double>(value, "cachedRemoteURLExpiresAt");
            entry.generationStatus = optionalValue<std::string>(value, "generationStatus");
            entry.importInput = optionalValue<JsonValue>(value, "importInput");
            entries.push_back(std::move(entry));
        }
        std::vector<MediaFolder> folders;
        std::unordered_set<std::string> folderIds;
        for (const json& value : root.value("folders", json::array())) {
            MediaFolder folder;
            folder.id = value.at("id").get<std::string>();
            folder.name = value.at("name").get<std::string>();
            folder.parentFolderId = optionalValue<std::string>(value, "parentFolderId");
            if (folder.id.empty() || !folderIds.insert(folder.id).second)
                throw std::runtime_error("invalid or duplicated media folder");
            folders.push_back(std::move(folder));
        }
        MediaManifest parsed;
        parsed.version_ = version;
        parsed.entries_ = std::move(entries);
        parsed.folders_ = std::move(folders);
        std::string validationError;
        if (!parsed.validate(&validationError)) throw std::runtime_error(validationError);
        *this = std::move(parsed);
        return true;
    } catch (const std::exception& exception) {
        setError(error, std::string("Invalid Palmier media JSON: ") + exception.what());
        return false;
    }
}

std::string MediaManifest::toJson() const {
    json entries = json::array();
    for (const MediaManifestEntry& entry : entries_) {
        json object = {{"id", entry.id}, {"name", entry.name}, {"type", toString(entry.type)},
                       {"source", sourceJson(entry.source)}, {"duration", entry.duration}};
        putOptional(object, "generationInput", entry.generationInput);
        putOptional(object, "sourceWidth", entry.sourceWidth);
        putOptional(object, "sourceHeight", entry.sourceHeight);
        putOptional(object, "sourceFPS", entry.sourceFPS);
        putOptional(object, "hasAudio", entry.hasAudio);
        putOptional(object, "folderId", entry.folderId);
        putOptional(object, "cachedRemoteURL", entry.cachedRemoteURL);
        putOptional(object, "cachedRemoteURLExpiresAt", entry.cachedRemoteURLExpiresAt);
        putOptional(object, "generationStatus", entry.generationStatus);
        putOptional(object, "importInput", entry.importInput);
        entries.push_back(std::move(object));
    }
    json folders = json::array();
    for (const MediaFolder& folder : folders_) {
        json object = {{"id", folder.id}, {"name", folder.name}};
        putOptional(object, "parentFolderId", folder.parentFolderId);
        folders.push_back(std::move(object));
    }
    return json{{"version", 2}, {"entries", std::move(entries)}, {"folders", std::move(folders)}}.dump();
}

bool MediaManifest::validate(std::string* error) const {
    if (version_ != 2) { setError(error, "media manifest version must be 2"); return false; }
    std::unordered_set<std::string> ids;
    for (const MediaManifestEntry& entry : entries_) {
        if (entry.id.empty() || !ids.insert(entry.id).second || entry.source.path.empty() ||
            !std::isfinite(entry.duration) || entry.duration < 0 ||
            (entry.sourceFPS && (!std::isfinite(*entry.sourceFPS) || *entry.sourceFPS <= 0)) ||
            (entry.cachedRemoteURLExpiresAt && !std::isfinite(*entry.cachedRemoteURLExpiresAt)) ||
            (entry.sourceWidth && *entry.sourceWidth <= 0) || (entry.sourceHeight && *entry.sourceHeight <= 0)) {
            setError(error, "media manifest contains an invalid or duplicated entry"); return false;
        }
    }
    std::unordered_set<std::string> folderIds;
    for (const MediaFolder& folder : folders_) {
        if (folder.id.empty() || !folderIds.insert(folder.id).second) {
            setError(error, "media manifest contains an invalid or duplicated folder"); return false;
        }
    }
    return true;
}

bool MediaManifest::add(MediaManifestEntry entry, std::string* error) {
    if (entry.id.empty() || find(entry.id)) { setError(error, "media id is empty or already exists"); return false; }
    if (entry.source.path.empty() || !std::isfinite(entry.duration) || entry.duration < 0 ||
        (entry.sourceFPS && (!std::isfinite(*entry.sourceFPS) || *entry.sourceFPS <= 0)) ||
        (entry.cachedRemoteURLExpiresAt && !std::isfinite(*entry.cachedRemoteURLExpiresAt))) {
        setError(error, "media source and duration must be valid"); return false;
    }
    entries_.push_back(std::move(entry));
    return true;
}

const MediaManifestEntry* MediaManifest::find(const std::string& id) const {
    for (const MediaManifestEntry& entry : entries_) if (entry.id == id) return &entry;
    return nullptr;
}

} // namespace heisenberg
