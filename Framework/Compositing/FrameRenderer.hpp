#pragma once

#include <Common/VideoFrame.hpp>
#include <Models/TimelineSettings.hpp>

#include <memory>
#include <string>

namespace heisenberg {

// Offscreen renderer, independent of window/swapchain lifetime. It owns no
// thread. The preview must use this instance exclusively on its render worker
// and call shutdown there before stopping that worker. Construction and an
// uninitialized CPU background need no GPU/device initialization.
class FrameRenderer {
public:
    explicit FrameRenderer(TimelineSettings settings = TimelineSettings::hd1080p24());
    ~FrameRenderer();
    FrameRenderer(const FrameRenderer&) = delete;
    FrameRenderer& operator=(const FrameRenderer&) = delete;

    void setSettings(TimelineSettings settings);
    const TimelineSettings& settings() const;
    bool initialize(std::string* error = nullptr);
    bool isReady() const;
    void shutdown();

    // Timeline mode converts to settings' size/SAR/color/working format.
    // Source mode deliberately bypasses normalization and retains source PTS.
    std::shared_ptr<VideoFrame> render(const std::shared_ptr<VideoFrame>& source,
                                      int64_t position, std::string* error = nullptr);
    std::shared_ptr<VideoFrame> background(int64_t position,
                                          std::string* error = nullptr);
    static std::shared_ptr<VideoFrame> passthrough(
        const std::shared_ptr<VideoFrame>& source, int64_t position);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace heisenberg
