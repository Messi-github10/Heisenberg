#include "EditorViewModel.hpp"

#include <Preview/Producer.hpp>
#include <Utiles/Logger.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <unordered_map>

namespace heisenberg {
namespace {

void setError(std::string* error, const std::string& message) {
    if (error) *error = message;
}

std::string utf8FromPath(const std::filesystem::path& path) {
    const auto utf8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(utf8.c_str()), utf8.size());
}

std::string fileStem(const std::string& path) {
    return utf8FromPath(std::filesystem::u8path(path).stem());
}

std::string makeAssetId(const MediaManifest& manifest) {
    for (int64_t index = 1;; ++index) {
        const std::string id = "media_" + std::to_string(index);
        if (!manifest.find(id)) return id;
    }
}

bool writeTextFile(const std::filesystem::path& path,
                   const std::string& text,
                   std::string* error) {
    std::error_code code;
    std::filesystem::create_directories(path.parent_path(), code);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        setError(error, "Failed to write '" + utf8FromPath(path) + "'");
        return false;
    }
    file << text;
    return true;
}

bool readTextFile(const std::filesystem::path& path,
                  std::string& text,
                  std::string* error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        setError(error, "Failed to open '" + utf8FromPath(path) + "'");
        return false;
    }
    text.assign((std::istreambuf_iterator<char>(file)),
                std::istreambuf_iterator<char>());
    return true;
}

int64_t inspectDuration(const std::string& path,
                        const Profile& profile,
                        bool hardwareDecode,
                        std::string* error) {
    Producer producer(profile);
    producer.setHardwareDecode(hardwareDecode);
    if (!producer.open(path, error)) return -1;
    return producer.length();
}

} // namespace

struct EditorViewModel::Impl {
    Profile profile = Profile::hd1080p24();
    bool hardwareDecode = false;
    std::string projectDir;
    MediaManifest manifest;
    Timeline timeline;

    std::unordered_map<std::string, std::string> mediaPaths() const {
        std::unordered_map<std::string, std::string> paths;
        MediaResolver resolver(manifest, projectDir);
        for (const MediaManifestEntry& entry : manifest.entries()) {
            const std::string path = resolver.resolvePath(entry.id);
            if (!path.empty()) paths.emplace(entry.id, path);
        }
        return paths;
    }
};

EditorViewModel::EditorViewModel()
    : impl_(std::make_unique<Impl>()) {}

EditorViewModel::~EditorViewModel() = default;

void EditorViewModel::setHardwareDecode(bool enabled) {
    impl_->hardwareDecode = enabled;
    impl_->timeline.setHardwareDecode(enabled);
}

void EditorViewModel::setProfile(Profile profile) {
    impl_->profile = std::move(profile);
    impl_->timeline.setProfile(impl_->profile);
}

bool EditorViewModel::importMedia(const std::string& path, std::string* error) {
    if (path.empty()) {
        setError(error, "media path must not be empty");
        return false;
    }
    std::error_code code;
    if (!std::filesystem::exists(std::filesystem::u8path(path), code)) {
        setError(error, "media file '" + path + "' was not found");
        return false;
    }

    const int64_t duration = inspectDuration(
        path, impl_->profile, impl_->hardwareDecode, error);
    if (duration < 0) return false;

    MediaManifestEntry entry;
    entry.id = makeAssetId(impl_->manifest);
    entry.name = fileStem(path);
    entry.type = MediaType::Video;
    entry.source.kind = MediaSource::Kind::External;
    entry.source.path = path;
    entry.durationFrames = duration;
    if (!impl_->manifest.add(entry, error)) return false;
    LOG_INFO("EditorViewModel: imported {} as {}", path, entry.id);
    return true;
}

std::string EditorViewModel::placeClip(const std::string& mediaRef,
                                       int64_t start,
                                       int64_t in,
                                       int64_t out,
                                       std::string* error) {
    MediaResolver lookup(impl_->manifest, impl_->projectDir);
    const MediaAsset asset = lookup.asset(mediaRef);
    if (asset.missing) {
        setError(error, "mediaRef '" + mediaRef + "' is missing");
        return {};
    }

    Clip clip;
    clip.mediaRef = mediaRef;
    clip.resource = asset.path;
    clip.start = start;
    clip.in = in;
    if (out >= 0) {
        clip.out = out;
    } else if (asset.durationFrames > 0) {
        clip.out = asset.durationFrames - 1;
    } else {
        setError(error, "mediaRef '" + mediaRef + "' has empty duration");
        return {};
    }
    if (!impl_->timeline.addClip(clip, error)) return {};
    return clip.id;
}

bool EditorViewModel::removeClip(const std::string& clipId, std::string* error) {
    return impl_->timeline.removeClip(clipId, error);
}

bool EditorViewModel::moveClip(const std::string& clipId, int64_t start, std::string* error) {
    return impl_->timeline.moveClip(clipId, start, error);
}

bool EditorViewModel::load(const std::string& projectDir, std::string* error) {
    const auto dir = std::filesystem::u8path(projectDir);
    std::string manifestText;
    std::string timelineText;
    if (!readTextFile(dir / "media.json", manifestText, error) ||
        !readTextFile(dir / "timeline.json", timelineText, error)) {
        return false;
    }

    MediaManifest manifest;
    if (!manifest.loadFromJson(manifestText, error)) return false;

    MediaResolver lookup(manifest, utf8FromPath(dir));
    std::unordered_map<std::string, std::string> paths;
    for (const MediaManifestEntry& entry : manifest.entries()) {
        const std::string path = lookup.resolvePath(entry.id);
        if (!path.empty()) paths.emplace(entry.id, path);
    }

    Timeline timeline;
    timeline.setHardwareDecode(impl_->hardwareDecode);
    if (!timeline.loadFromJson(timelineText, error, utf8FromPath(dir), paths)) {
        return false;
    }

    impl_->projectDir = utf8FromPath(dir);
    impl_->manifest = std::move(manifest);
    impl_->timeline = std::move(timeline);
    impl_->profile = impl_->timeline.profile();
    return true;
}

bool EditorViewModel::save(const std::string& projectDir, std::string* error) const {
    const auto dir = std::filesystem::u8path(projectDir);
    return writeTextFile(dir / "media.json", impl_->manifest.toJson(), error) &&
           writeTextFile(dir / "timeline.json", impl_->timeline.toJson(), error);
}

const MediaManifest& EditorViewModel::manifest() const {
    return impl_->manifest;
}

MediaResolver EditorViewModel::resolver() const {
    return MediaResolver(impl_->manifest, impl_->projectDir);
}

const Timeline& EditorViewModel::timeline() const {
    return impl_->timeline;
}

Timeline& EditorViewModel::timeline() {
    return impl_->timeline;
}

std::vector<MediaAsset> EditorViewModel::assets() const {
    std::vector<MediaAsset> assets;
    MediaResolver lookup(impl_->manifest, impl_->projectDir);
    assets.reserve(impl_->manifest.entries().size());
    for (const MediaManifestEntry& entry : impl_->manifest.entries()) {
        assets.push_back(lookup.asset(entry.id));
    }
    return assets;
}

const std::string& EditorViewModel::projectDir() const {
    return impl_->projectDir;
}

} // namespace heisenberg
