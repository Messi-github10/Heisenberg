//
// Created by NiceFold on 2026/7/9.
//

#pragma once

#include <memory>
#include <functional>
#include <Common/NonCopy.hpp>

extern "C" {
#include <libplacebo/gpu.h>
#include <libplacebo/renderer.h>
#include <libplacebo/vulkan.h>
}

struct AVFrame;

namespace heisenberg {

namespace filtergraph {
    class IInputNode;
    class IOutputNode;
    class IPipeGraph;
    struct VulkanImageRef;
}

namespace renderer {

class SwapChain;
class TextureManager;
class RenderEngine;

class IPreviewer : public NonCopy {
public:
    using ResizeCallback  = std::function<void(int width, int height)>;
    using PresentCallback = std::function<void()>;

    IPreviewer();
    ~IPreviewer();

    IPreviewer(IPreviewer&&)            = delete;
    IPreviewer& operator=(IPreviewer&&) = delete;

    bool initialize(pl_gpu gpu, pl_vulkan vk,
                    std::unique_ptr<SwapChain> swapChain,
                    int width, int height);

    bool presentFrame(const AVFrame* frame);

    void resize(int width, int height);

    void setOnResize(ResizeCallback cb);
    void setOnPresent(PresentCallback cb);

    void setFilterGraph(heisenberg::filtergraph::IPipeGraph* graph,
                        heisenberg::filtergraph::IInputNode* input,
                        heisenberg::filtergraph::IOutputNode* output);

    void shutdown();

private:
    bool buildIntermediateTarget(int width, int height);
    void releaseIntermediateTarget();
    bool renderToSwapChain(const pl_frame* source, int width, int height);
    bool renderToSwapChain(const filtergraph::VulkanImageRef& image);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace renderer
} // namespace heisenberg
