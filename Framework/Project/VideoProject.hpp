#pragma once

#include <Editor/EditorViewModel.hpp>
#include "ProjectPackageCoordinator.hpp"

namespace heisenberg {

// An owned, immutable document capture. Resolvers built from it never borrow the
// live editor's manifest, and the package root travels with project-relative media.
class ProjectSnapshot {
public:
    const ProjectFile& project() const {
        return project_;
    }
    const MediaManifest& manifest() const {
        return manifest_;
    }
    const std::string& projectDir() const {
        return root_;
    }
    const std::string& packageRoot() const {
        return root_;
    }
    uint64_t revision() const {
        return revision_;
    }
    MediaResolver resolver() const {
        return MediaResolver(manifest_, root_);
    }
    const Timeline* activeTimeline() const {
        return project_.activeTimeline();
    }
private:
    friend class VideoProject;
    ProjectFile project_;
    MediaManifest manifest_;
    std::string root_;
    uint64_t revision_ = 0;
    uint64_t documentRevision_ = 0;
    bool transient_ = false;
    uint64_t generation_ = 0;
    std::string ownerId_;
};

class VideoProject {
public:
    VideoProject();
    VideoProject(const VideoProject&) = delete;
    VideoProject& operator=(const VideoProject&) = delete;
    EditorViewModel& editor() {
        return editor_;
    }
    const EditorViewModel& editor() const {
        return editor_;
    }
    uint64_t revision() const {
        return editor_.revision();
    }
    uint64_t savedRevision() const {
        return savedRevision_;
    }
    bool isDirty() const {
        return editor_.documentRevision() != savedDocumentRevision_;
    }
    ProjectSnapshot snapshot() const;
    bool loadPackage(const std::string& path, std::string* error = nullptr);
    bool savePackage(const std::string& path, std::string* error = nullptr);
    bool save(std::string* error = nullptr);
    bool saveAs(const std::string& path, std::string* error = nullptr);
    // Persist a previously captured revision. A newer committed document stays
    // dirty. Saves/loads are rejected during a pending effects transaction;
    // snapshots captured during one can never be persisted.
    bool saveSnapshot(const ProjectSnapshot& snapshot, const std::string& path,
                      std::string* error = nullptr);
private:
    EditorViewModel editor_;
    ProjectPackageCoordinator coordinator_;
    uint64_t savedRevision_ = 0;
    uint64_t savedDocumentRevision_ = 0;
    uint64_t generation_ = 0;
    std::string ownerId_;
};

} // namespace heisenberg
