#pragma once

#include "CompositionPlan.hpp"
#include <Models/MediaManifest.hpp>
#include <Models/Timeline.hpp>

namespace heisenberg {

class ProjectSnapshot;

class CompositionBuilder {
public:
    static bool build(const Timeline& timeline, const MediaManifest& manifest,
                      const std::string& projectDir, CompositionPlan& result,
                      std::string* error = nullptr);
    static bool build(const Timeline& timeline, const MediaManifest& manifest,
                      const std::string& projectDir, const ColorSettings& colorSettings,
                      CompositionPlan& result, std::string* error = nullptr);
    static bool build(const ProjectSnapshot& snapshot, CompositionPlan& result,
                      std::string* error = nullptr);
};

} // namespace heisenberg
