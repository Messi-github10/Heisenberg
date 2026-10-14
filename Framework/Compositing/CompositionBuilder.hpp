#pragma once

#include "CompositionPlan.hpp"
#include <Models/MediaManifest.hpp>
#include <Models/Timeline.hpp>

namespace heisenberg {

class CompositionBuilder {
public:
    // Pure snapshot construction: paths are captured, not opened or probed.
    static bool build(const Timeline& timeline, const MediaManifest& manifest,
                      const std::string& projectDir, CompositionPlan& result,
                      std::string* error = nullptr);
};

} // namespace heisenberg
