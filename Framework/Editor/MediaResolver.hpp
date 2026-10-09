#pragma once

#include "MediaManifest.hpp"

#include <string>

namespace heisenberg {

class MediaResolver {
public:
    MediaResolver(const MediaManifest& manifest, std::string projectDir = {});

    std::string expectedPath(const std::string& assetId) const;
    std::string resolvePath(const std::string& assetId) const;
    bool isMissing(const std::string& assetId) const;
    std::string displayName(const std::string& assetId) const;
    const MediaManifestEntry* entry(const std::string& assetId) const;
    MediaAsset asset(const std::string& assetId) const;

private:
    const MediaManifest* manifest_ = nullptr;
    std::string projectDir_;
};

} // namespace heisenberg
