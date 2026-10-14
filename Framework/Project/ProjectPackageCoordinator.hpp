#pragma once

#include <Models/ProjectFile.hpp>
#include <Models/MediaManifest.hpp>

namespace heisenberg {
class ProjectSnapshot;

// Stateless disk boundary. Successful reads are installed by VideoProject;
// successful writes checkpoint only the captured revision there.
class ProjectPackageCoordinator {
public:
    bool loadPackage(const std::string& path, ProjectFile& project, MediaManifest& manifest,
                     std::string& canonicalRoot, std::string* error = nullptr) const;
    bool savePackage(const ProjectSnapshot& snapshot, const std::string& path,
                     std::string& canonicalRoot, std::string* error = nullptr) const;
};

} // namespace heisenberg
