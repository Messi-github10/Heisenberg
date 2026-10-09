#include "MediaResolver.hpp"

#include <filesystem>

namespace heisenberg {
namespace {

std::string utf8FromPath(const std::filesystem::path& path) {
    const auto utf8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(utf8.c_str()), utf8.size());
}

bool fileExists(const std::string& path) {
    if (path.empty()) return false;
    std::error_code code;
    return std::filesystem::exists(std::filesystem::u8path(path), code);
}

} // namespace

MediaResolver::MediaResolver(const MediaManifest& manifest, std::string projectDir)
    : manifest_(&manifest)
    , projectDir_(std::move(projectDir)) {}

std::string MediaResolver::expectedPath(const std::string& assetId) const {
    const MediaManifestEntry* found = entry(assetId);
    if (!found) return {};
    if (found->source.kind == MediaSource::Kind::External) return found->source.path;
    if (projectDir_.empty()) return {};
    return utf8FromPath(std::filesystem::u8path(projectDir_) /
                        std::filesystem::u8path(found->source.path));
}

std::string MediaResolver::resolvePath(const std::string& assetId) const {
    const std::string path = expectedPath(assetId);
    return fileExists(path) ? path : std::string{};
}

bool MediaResolver::isMissing(const std::string& assetId) const {
    return resolvePath(assetId).empty();
}

std::string MediaResolver::displayName(const std::string& assetId) const {
    const MediaManifestEntry* found = entry(assetId);
    if (!found) return "Offline";
    return found->name.empty() ? assetId : found->name;
}

const MediaManifestEntry* MediaResolver::entry(const std::string& assetId) const {
    return manifest_ ? manifest_->find(assetId) : nullptr;
}

MediaAsset MediaResolver::asset(const std::string& assetId) const {
    MediaAsset result;
    const MediaManifestEntry* found = entry(assetId);
    if (!found) {
        result.id = assetId;
        result.name = "Offline";
        result.missing = true;
        return result;
    }
    result.id = found->id;
    result.name = found->name;
    result.type = found->type;
    result.path = expectedPath(assetId);
    result.durationFrames = found->durationFrames;
    result.missing = !fileExists(result.path);
    return result;
}

} // namespace heisenberg
