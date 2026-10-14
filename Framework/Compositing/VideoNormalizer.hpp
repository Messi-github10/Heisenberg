#pragma once

#include <Models/TimelineSettings.hpp>

#include <memory>
#include <string>

struct AVFrame;

namespace heisenberg {

// Per-renderer offscreen working-format conversion. No cache/singleton and no
// additional worker: initialization, normalization and shutdown must all occur
// on the application's render worker. AVFrames returned own their pixel storage.
class VideoNormalizer {
public:
    VideoNormalizer();
    ~VideoNormalizer();
    VideoNormalizer(const VideoNormalizer&) = delete;
    VideoNormalizer& operator=(const VideoNormalizer&) = delete;

    bool initialize(const TimelineSettings& settings, std::string* error = nullptr);
    std::shared_ptr<AVFrame> normalize(const AVFrame* source,
                                       std::string* error = nullptr);
    void shutdown();
    bool isReady() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace heisenberg
