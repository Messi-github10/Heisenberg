#pragma once

#include <Common/VideoSpec.hpp>
#include <MultiMedia/Video/Renderer/FilterGraph/Common/FilterCommon.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace heisenberg::renderer::color {

inline constexpr const char* kConfigId = "studio-config-v4.0.0_aces-v2.0_ocio-v2.5";
inline constexpr const char* kWorkingSpace = "ACEScg";
inline constexpr const char* kGradingSpace = "ACEScct";
inline constexpr const char* kPreviewDisplay = "sRGB - Display";
inline constexpr const char* kExportDisplay = "Rec.1886 Rec.709 - Display";
inline constexpr const char* kColorimetricView = "Un-tone-mapped";
inline constexpr const char* kAcesOutputView = "ACES 2.0 - SDR 100 nits (Rec.709)";
inline constexpr const char* kOutputView = kColorimetricView;

enum class InputSelectionSource { Override, Metadata, Fallback };
struct ColorInputResolve {
    VideoSpec observedMetadata;
    std::string overrideSpace;
    std::string colorSpace;
    InputSelectionSource source = InputSelectionSource::Fallback;
    std::vector<std::string> diagnostics;
};

// Backend-neutral program data. No OCIO types or policy enter FilterGraph.
// Set 0: binding 0 sampled input, binding 1 storage output.
// Set 1: binding 0 std140 UBO, LUT bindings as supplied (starting at 1).
struct GpuLut {
    uint32_t binding = 0;
    uint32_t width = 0, height = 0, depth = 1;
    uint32_t channels = 3;
    bool is3D = false;
    bool linearInterpolation = true;
    std::vector<float> values;
};
struct GpuProgram {
    // Immutable identity of shader, LUT contents/bindings and UBO layout.
    // Changing any of those requires a new ID; UBO values may change in place.
    std::string cacheId;
    std::string shaderText; // OCIO function and declarations, not a complete compute shader
    std::string functionName;
    std::vector<std::byte> uniformData;
    std::vector<GpuLut> luts;
    // Optional dynamic UBO snapshot provider; shader/LUT identity stays fixed.
    std::function<std::vector<std::byte>()> uniformUpdate;
};

inline constexpr size_t kProgramCacheCapacity = 16;
struct ColorGpuCacheStats {
    uint64_t hits = 0, misses = 0, shaderCompilations = 0, lutUploads = 0, evictions = 0;
    size_t residentPrograms = 0;
};

class ColorGpuPass;
class ColorProcessor {
public:
    ColorProcessor();
    ~ColorProcessor();
    ColorProcessor(const ColorProcessor&);
    ColorProcessor& operator=(const ColorProcessor&);
    explicit operator bool() const;
    // CPU reference only: never used as a GPU execution fallback. Alpha preserved.
    void applyRGBA(float* pixels, size_t pixelCount) const;
    GpuProgram gpuProgram() const;
    // Shared immutable shader/LUT snapshot; dynamic UBO values are read by
    // its uniformUpdate callback at execution time without copying LUTs.
    std::shared_ptr<const GpuProgram> gpuProgramSnapshot() const;
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
    friend class ColorPipeline;
};

// Synchronous GPU submissions retain no borrowed images: completion is waited
// before execute returns; ready is a persistent timeline semaphore. Caller owns
// output allocation/lease and must set its layout to SHADER_READ_ONLY_OPTIMAL.
// Device/context must outlive this object. This pass/pipeline must outlive
// downstream submissions using its returned semaphore. An owned-frame renderer
// may host-wait ready, then publish its own lease-owned completion semaphore.
class ColorGpuPass {
public:
    explicit ColorGpuPass(filtergraph::VulkanGraphContext context);
    ~ColorGpuPass();
    ColorGpuPass(const ColorGpuPass&) = delete;
    ColorGpuPass& operator=(const ColorGpuPass&) = delete;
    // Callback must be thread-safe. Setting it is serialized with execute;
    // request cancellation by changing the token observed by the callback.
    // A submitted pass is always drained before throwing "Cancelled".
    void setCancellation(std::function<bool()> cancellation);
    ColorGpuCacheStats cacheStats() const;
    filtergraph::VulkanSyncPoint execute(const GpuProgram& program,
        const filtergraph::VulkanImageRef& input,
        const filtergraph::VulkanImageRef& output);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class ColorPipeline {
public:
    explicit ColorPipeline(filtergraph::VulkanGraphContext context, const std::string& configId = kConfigId);
    // CPU-only config/processor access, with no Vulkan object creation.
    ColorPipeline();
    ~ColorPipeline();
    ColorPipeline(const ColorPipeline&) = delete;
    ColorPipeline& operator=(const ColorPipeline&) = delete;
    void setCancellation(std::function<bool()> cancellation);
    // Policy 1 preserves scene-referred legacy mappings; policy 2 interprets
    // ordinary SDR video as finished, display-referred material. Overrides win.
    ColorInputResolve resolveInput(const VideoSpec& metadata,
                                  const std::string& overrideSpace = {},
                                  int inputPolicyVersion = 2) const;
    ColorProcessor processor(const std::string& source, const std::string& destination) const;
    // A file LUT operates in its declared grading domain; caller brackets it
    // with ACEScg <-> ACEScct when that is the LUT's input/output encoding.
    ColorProcessor lutProcessor(const std::string& filename) const;
    ColorProcessor displayProcessor(const std::string& source = kWorkingSpace,
        const std::string& display = kPreviewDisplay, const std::string& view = kOutputView) const;
    filtergraph::VulkanSyncPoint executeSpace(const filtergraph::VulkanImageRef& input,
        const filtergraph::VulkanImageRef& output,
        const std::string& source, const std::string& destination, std::string* error = nullptr);
    filtergraph::VulkanSyncPoint executeDisplay(const filtergraph::VulkanImageRef& input,
        const filtergraph::VulkanImageRef& output, const std::string& source = kWorkingSpace,
        const std::string& display = kPreviewDisplay, const std::string& view = kOutputView,
        std::string* error = nullptr);
    std::vector<std::string> colorSpaces() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace heisenberg::renderer::color
