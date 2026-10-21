#include "ProjectPackageCoordinator.hpp"
#include "VideoProject.hpp"

#include <Models/ProjectCodec.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <cwctype>

namespace heisenberg {
namespace {
namespace fs = std::filesystem;
void setError(std::string* error, const std::string& message) {
    if (error) *error = message;
}
std::string utf8(const fs::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.c_str()), value.size()};
}
fs::path packagePath(const std::string& text) {
    if (text.empty()) throw std::runtime_error("package directory is required");
    auto path = fs::u8path(text);
    if (path.filename() == "project.json") path = path.parent_path();
    return fs::weakly_canonical(fs::absolute(path));
}
bool sameComponent(const fs::path& left, const fs::path& right) {
#ifdef _WIN32
    auto a = left.native(), b = right.native();
    for (auto& c : a) c = static_cast<wchar_t>(std::towlower(c));
    for (auto& c : b) c = static_cast<wchar_t>(std::towlower(c));
    return a == b;
#else
    return left == right;
#endif
}
bool nested(const fs::path& child, const fs::path& parent) {
    auto c = child.begin();
    for (auto p = parent.begin(); p != parent.end(); ++p, ++c) {
        if (c == child.end() || !sameComponent(*c, *p)) return false;
    }
    return true;
}
std::string readText(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Failed to open '" + utf8(path) + "'");
    std::string text{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    if (stream.bad()) throw std::runtime_error("Failed to read '" + utf8(path) + "'");
    return text;
}
void writeText(const fs::path& path, const std::string& text) {
    // Do not follow preserved symlinks for the two authoritative documents.
    if (fs::is_symlink(fs::symlink_status(path))) fs::remove(path);
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) throw std::runtime_error("Failed to stage '" + utf8(path) + "'");
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    stream.flush();
    if (!stream) throw std::runtime_error("Failed to stage '" + utf8(path) + "'");
    stream.close();
    if (!stream) throw std::runtime_error("Failed to close staged '" + utf8(path) + "'");
}
void copyContents(const fs::path& source, const fs::path& destination) {
    for (const auto& entry : fs::directory_iterator(source)) {
        const auto target = destination / entry.path().filename();
        // A directory copy recursively retains unknown package files and media.
        // Conflicting types are replaced only in our private staging directory.
        const auto sourceStatus = entry.symlink_status();
        const auto targetStatus = fs::symlink_status(target);
        if (fs::exists(targetStatus) &&
            (sourceStatus.type() != targetStatus.type() || fs::is_symlink(targetStatus)))
            fs::remove_all(target);
        fs::copy(entry.path(), target, fs::copy_options::recursive |
                 fs::copy_options::overwrite_existing | fs::copy_options::copy_symlinks);
    }
}
struct StagingDirectory {
    fs::path path;
    ~StagingDirectory() { std::error_code ignored; fs::remove_all(path, ignored); }
};

bool persist(const ProjectSnapshot& snapshot, const fs::path& destination, std::string* error) {
    if (!validateProject(snapshot.project(), error) || !snapshot.manifest().validate(error)) return false;
    try {
        const fs::path source = snapshot.projectDir().empty() ? fs::path{} : packagePath(snapshot.projectDir());
        const bool same = !source.empty() && nested(source, destination) && nested(destination, source);
        if (!source.empty() && !same && (nested(destination, source) || nested(source, destination)))
            throw std::runtime_error("Save As source and destination packages cannot contain one another");
        if (fs::exists(destination) && !fs::is_directory(destination))
            throw std::runtime_error("package destination must be a directory");
        if (!source.empty() && !same && !fs::is_directory(source))
            throw std::runtime_error("source package is unavailable; cannot preserve its media and unknown files");

        // Stage the complete package next to the destination, on the same volume.
        // Nothing authoritative changes until BOTH documents have been closed.
        fs::create_directories(destination.parent_path());
        StagingDirectory staging{destination.parent_path() / (".heisenberg-stage-" + makeUuid())};
        fs::create_directory(staging.path);
        if (fs::is_directory(destination)) copyContents(destination, staging.path);
        if (!source.empty() && !same) copyContents(source, staging.path);
        const auto mediaPath = staging.path / "media";
        if (!fs::exists(mediaPath)) fs::create_directory(mediaPath);
        if (!fs::is_directory(mediaPath)) throw std::runtime_error("package media path must be a directory");
        writeText(staging.path / "project.json", ProjectCodec::encode(snapshot.project()));
        writeText(staging.path / "media.json", snapshot.manifest().toJson());

        const fs::path backup = destination.parent_path() / (".heisenberg-backup-" + makeUuid());
        const bool existed = fs::exists(destination);
        if (existed) fs::rename(destination, backup);
        std::error_code commitError;
        fs::rename(staging.path, destination, commitError);
        if (commitError) {
            if (existed) {
                std::error_code rollbackError;
                fs::rename(backup, destination, rollbackError);
                if (rollbackError) {
                    setError(error, "Package replacement failed; original package preserved at '" +
                             utf8(backup) + "', but restore failed: " + rollbackError.message());
                    return false;
                }
            }
            throw std::runtime_error("Failed to replace package: " + commitError.message());
        }
        if (existed) { std::error_code ignored; fs::remove_all(backup, ignored); }
        return true;
    } catch (const std::exception& exception) {
        setError(error, exception.what());
        return false;
    }
}
} // namespace

bool ProjectPackageCoordinator::loadPackage(const std::string& path, ProjectFile& resultProject,
                                             MediaManifest& resultManifest, std::string& canonicalRoot,
                                             std::string* error) const {
    try {
        const auto root = packagePath(path);
        if (!fs::is_directory(root)) throw std::runtime_error("Berg package directory was not found");
        ProjectFile project;
        MediaManifest manifest;
        if (!ProjectCodec::decode(readText(root / "project.json"), project, error) ||
            !manifest.loadFromJson(readText(root / "media.json"), error)) return false;
        resultProject = std::move(project);
        resultManifest = std::move(manifest);
        canonicalRoot = utf8(root);
        return true;
    } catch (const std::exception& exception) {
        setError(error, exception.what());
        return false;
    }
}
bool ProjectPackageCoordinator::savePackage(const ProjectSnapshot& snapshot, const std::string& path,
                                             std::string& canonicalRoot, std::string* error) const {
    try {
        const auto root = packagePath(path);
        if (!persist(snapshot, root, error)) return false;
        canonicalRoot = utf8(root);
        return true;
    } catch (const std::exception& exception) {
        setError(error, exception.what());
        return false;
    }
}

} // namespace heisenberg
