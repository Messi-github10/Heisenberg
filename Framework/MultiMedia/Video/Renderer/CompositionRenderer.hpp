#pragma once

#include <Common/VideoFrame.hpp>
#include <Models/TimelineSettings.hpp>
#include <MultiMedia/Video/Renderer/FilterGraph/Common/FilterCommon.hpp>
#include <MultiMedia/Video/Renderer/FilterGraph/Vulkan/Graph/VulkanGraphDocument.hpp>
#include <cstdint>
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
namespace renderer {

enum class ColorOutputIntent { Colorimetric, SdrVideoPreview };

struct ColorOutputRequest {
    // Empty selects sRGB - Display / Un-tone-mapped, independently of policy.
    std::string display;
    std::string view;
    // For a direct encoded colorspace transform instead of a display view.
    std::string colorSpace;
    // Preview may preserve finished Rec.709 code values on an SDR sRGB surface.
    // Export and explicit colorimetric outputs never opt into that presentation intent.
    ColorOutputIntent intent = ColorOutputIntent::Colorimetric;
};

// A lease owns the image, not merely a borrowed graph/interop view. Copies keep
// the allocation alive. Consumers wait on image().ready before reading and call
// release(done) after submitting their last read. Destruction waits for both.
class RenderedFrame {
public:
    RenderedFrame() = default;
    explicit operator bool() const;
    filtergraph::VulkanImageRef image() const;
    int64_t position() const;
    bool encoded() const;
    // Actual resolved encoding of output(); empty for scene-linear working frames.
    const ColorOutputRequest& outputSelectionMetadata() const;
    void release(filtergraph::VulkanSyncPoint done) const;
private:
    struct Storage;
    std::shared_ptr<Storage> storage_;
    friend class CompositionRenderer;
};

struct CompositionRenderRequest {
    const CompositionPlan* plan = nullptr;
    std::shared_ptr<VideoFrame> source;
    TimelineSettings settings = TimelineSettings::hd1080p24();
    int64_t targetFrame = 0;
    int64_t clipLocalFrame = 0;
    std::string clipId;
    bool sourceMode = false;
    bool blank = false;
    bool strict = false;
    std::vector<std::string>* diagnostics = nullptr;
    // Empty selects the metadata-derived OCIO input; an explicit IDT wins.
    std::string inputColorSpace;
    // Stable media identity for bounded, non-per-frame color-path diagnostics.
    // If absent, the renderer uses the composition clip's mediaRef/sourcePath.
    std::string sourceId;
};

enum class DownloadFormat { RgbaFloat, Rgba64 };

// Windowless and threadless. One instance per preview/export render worker.
// All methods and final lease destruction belong to that worker. The external
// GpuContext/VkDevice must outlive this instance AND its outstanding leases.
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
    // Checked between stages and during polled waits. Cancellation drains any
    // submitted work before freeing GPU resources; it never abandons inflight work.
    void setCancellation(std::function<bool()> cancelled);
    RenderedFrame render(const CompositionRenderRequest&, std::string* error = nullptr);
    RenderedFrame render(const CompositionPlan&, int64_t targetFrame,
                         const std::shared_ptr<VideoFrame>& source,
                         std::string* error = nullptr);
    RenderedFrame output(const RenderedFrame&, const ColorOutputRequest& = {},
                         std::string* error = nullptr);
    std::shared_ptr<AVFrame> download(const RenderedFrame&, DownloadFormat,
                                    std::string* error = nullptr);
    // Value snapshot, including current parameter edits, portable to export.
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
