#pragma once

#include <Common/NonCopy.hpp>
#include "CompositionRenderer.hpp"
#include <functional>
#include <memory>

extern "C" {
#include <libplacebo/swapchain.h>
}

namespace heisenberg::renderer {
class SwapChain;
class RenderEngine;

// Shared final presentation path, also usable with a readable test attachment.
// Source is already OCIO output-encoded RGB; selection must describe its actual
// resolved encoding and presentation intent. Explicit SDR video preview keeps
// code values; Colorimetric keeps transfer adaptation. Target remains unmodified.
// Only SDR sRGB UNORM targets are supported; unknown/HDR/scRGB/implicit-sRGB
// attachments fail explicitly. No image ownership or synchronization is taken.
bool blitPresentationFrame(pl_gpu gpu, RenderEngine& blitter, pl_tex source,
                           const ColorOutputRequest& selection,
                           const pl_swapchain_frame& target,
                           std::string* error = nullptr);

// Presentation only. Never uploads decoded frames, executes a filter graph, or
// reapplies an OCIO view. Transfer adaptation is only between the declared
// encoded output and the actual supported SDR surface.
class VideoPresenter : public NonCopy {
public:
    using ResizeCallback = std::function<void(int, int)>;
    using PresentCallback = std::function<void()>;
    VideoPresenter();
    ~VideoPresenter();
    bool initialize(pl_gpu gpu, pl_vulkan vk, std::unique_ptr<SwapChain>, int width, int height);
    bool presentFrame(const RenderedFrame& encodedFrame);
    void resize(int width, int height);
    void setOnResize(ResizeCallback);
    void setOnPresent(PresentCallback);
    void shutdown();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace heisenberg::renderer
