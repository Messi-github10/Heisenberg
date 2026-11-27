#pragma once

#include <Common/VideoFrame.hpp>
#include <Compositing/CompositionExecutor.hpp>
#include <Models/TimelineSettings.hpp>
#include "FilterGraph/Common/FilterCommon.hpp"
#include <functional>
#include <memory>
#include <string>

extern "C" {
#include <libplacebo/gpu.h>
#include <libplacebo/vulkan.h>
}

struct ID3D11Device;
struct ID3D11DeviceContext;
namespace heisenberg {
struct CompositionPlan;
namespace filtergraph {
class VulkanGraphDocument;
}
namespace renderer {

enum class ColorOutputIntent {
    Colorimetric,
    CodePreserving
};

struct ColorOutputRequest {
    std::string display;
    std::string view;
    std::string colorSpace;
    ColorOutputIntent intent = ColorOutputIntent::Colorimetric;
};

class RenderedFrame {
public:
    RenderedFrame() = default;
    explicit operator bool() const;
    filtergraph::VulkanImageRef image() const;
    int64_t position() const;
    bool encoded() const;
    const ColorOutputRequest& outputSelectionMetadata() const;
    void release(filtergraph::VulkanSyncPoint done) const;

private:
    struct Storage;
    std::shared_ptr<Storage> storage_;
    friend class CompositionRenderer;
};

struct CompositionRenderRequest {
    const CompositionPlan* plan = nullptr;
    std::shared_ptr<const CompositionPlan> immutablePlan;
    std::shared_ptr<VideoFrame> source;
    std::vector<CompositionVideoLayer> layers;
    TimelineSettings settings = TimelineSettings::hd1080p24();
    int64_t targetFrame = 0;
    int64_t clipLocalFrame = 0;
    std::string clipId;
    bool sourceMode = false;
    bool blank = false;
    bool strict = false;
    std::vector<std::string>* diagnostics = nullptr;
    std::string inputColorSpace;
    std::string sourceId;
};

enum class DownloadFormat {
    RgbaFloat,
    Rgba64
};

class CompositionRenderer {
public:
    CompositionRenderer();
    ~CompositionRenderer();
    CompositionRenderer(const CompositionRenderer&) = delete;
    CompositionRenderer& operator=(const CompositionRenderer&) = delete;
    bool initialize(pl_gpu gpu, pl_vulkan vulkan,
                    ID3D11Device* device = nullptr,
                    ID3D11DeviceContext* context = nullptr,
                    std::string* error = nullptr);
    void shutdown();
    void setCancellation(std::function<bool()> cancelled);
    RenderedFrame render(const CompositionRenderRequest&, std::string* error = nullptr);
    RenderedFrame render(const CompositionPlan&, int64_t targetFrame,
                         const std::vector<CompositionVideoLayer>& layers,
                         std::string* error = nullptr);
    RenderedFrame render(const CompositionPlan&, int64_t targetFrame,
                         const std::shared_ptr<VideoFrame>& source,
                         std::string* error = nullptr);
    RenderedFrame output(const RenderedFrame&, const ColorOutputRequest& = {},
                         std::string* error = nullptr);
    std::shared_ptr<AVFrame> download(const RenderedFrame&, DownloadFormat,
                                    std::string* error = nullptr);
    bool setFilterGraphSnapshot(const filtergraph::VulkanGraphDocument&,
                                std::string* error = nullptr);
    filtergraph::VulkanGraphDocument filterGraphSnapshot() const;
    bool setNodeParameter(uint64_t nodeId, const std::string& name, float value);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace renderer
} // namespace heisenberg
