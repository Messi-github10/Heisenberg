#include "CompositionRenderer.hpp"
#include "Color/ColorPipeline.hpp"
#include "ColorSpaceUtils.hpp"
#include "RenderEngine.hpp"
#include "VulkanBlendPass.hpp"
#include <Compositing/CompositionPlan.hpp>
#include <Compositing/Grade/GradeCompiler.hpp>
#include <Models/ClipAppearance.hpp>
#include <Platform/Software/SoftwareContext.hpp>
#include <Platform/D3D11/D3D11VulkanInterop.hpp>
#include <Platform/Vulkan/VulkanQueueSync.hpp>
#include <MultiMedia/Video/Renderer/FilterGraph/Vulkan/Graph/VulkanFilterGraph.hpp>
#include <MultiMedia/Video/Renderer/FilterGraph/Vulkan/Resources/VulkanImageResource.hpp>
#include <MultiMedia/Video/Renderer/FilterGraph/Interface/INodeFactory.hpp>
#include <MultiMedia/Video/Renderer/FilterGraph/Interface/IPipeGraph.hpp>
#include <Utiles/Logger.hpp>
#include <volk.h>
extern "C" {
#include <libavutil/pixdesc.h>
#include <libplacebo/dispatch.h>
#include <libplacebo/filters.h>
#include <libplacebo/shaders/custom.h>
#include <libplacebo/shaders/sampling.h>
}
#include <d3d11.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace heisenberg::renderer {
namespace {
using namespace filtergraph;
constexpr VkImageUsageFlags imageUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
    | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
void wait(VkDevice device, VulkanSyncPoint point) {
    if (!point.valid()) return;
    VkSemaphoreWaitInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    info.semaphoreCount = 1; info.pSemaphores = &point.semaphore; info.pValues = &point.value;
    require(vkWaitSemaphores(device, &info, UINT64_MAX) == VK_SUCCESS, "Vulkan timeline wait failed");
}
pl_frame rgb(pl_tex texture, int w, int h) {
    pl_frame frame{}; frame.num_planes = 1;
    frame.planes[0].texture = texture; frame.planes[0].components = 4;
    for (int i = 0; i < 4; ++i) frame.planes[0].component_mapping[i] = i;
    frame.repr.sys = PL_COLOR_SYSTEM_RGB; frame.repr.levels = PL_COLOR_LEVELS_FULL;
    frame.repr.alpha = PL_ALPHA_INDEPENDENT; frame.color = workingColorSpace();
    frame.crop = {0, 0, static_cast<float>(w), static_cast<float>(h)}; return frame;
}
pl_tex wrap(pl_gpu gpu, const VulkanImageRef& image) {
    pl_vulkan_wrap_params params{}; params.image = image.image;
    params.width = static_cast<int>(image.extent.width); params.height = static_cast<int>(image.extent.height);
    params.format = image.vkFormat; params.usage = image.usage;
    auto tex = pl_vulkan_wrap(gpu, &params); require(tex != nullptr, "Cannot wrap renderer image");
    pl_vulkan_release_params release{}; release.tex = tex; release.layout = image.layout;
    release.qf = image.queueFamilyIndex; release.semaphore.sem = image.ready.semaphore; release.semaphore.value = image.ready.value;
    pl_vulkan_release_ex(gpu, &release); return tex;
}
// Every wrapper returns ownership before destruction, including exception and
// cancellation paths. Its private semaphore never re-signals a frame's lease.
class BorrowedTexture {
public:
    BorrowedTexture(pl_gpu gpu, const VulkanImageRef& image) : gpu_(gpu), image_(image) {
        pl_vulkan_sem_params params{}; params.type = VK_SEMAPHORE_TYPE_TIMELINE;
        done_ = pl_vulkan_sem_create(gpu_, &params);
        require(done_ != VK_NULL_HANDLE, "Cannot create image return semaphore");
        try { texture_ = wrap(gpu_, image_); }
        catch (...) { pl_vulkan_sem_destroy(gpu_, &done_); throw; }
    }
    ~BorrowedTexture() { close(); }
    BorrowedTexture(const BorrowedTexture&) = delete;
    BorrowedTexture& operator=(const BorrowedTexture&) = delete;
    pl_tex get() const { return texture_; }
    bool close() {
        if (!texture_) return returned_;
        pl_vulkan_hold_params params{}; params.tex = texture_;
        params.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        params.qf = image_.queueFamilyIndex; params.semaphore = {done_, 1};
        returned_ = pl_vulkan_hold_ex(gpu_, &params);
        pl_gpu_finish(gpu_);
        pl_tex_destroy(gpu_, &texture_); pl_vulkan_sem_destroy(gpu_, &done_);
        return returned_;
    }
private:
    pl_gpu gpu_; VulkanImageRef image_; pl_tex texture_ = nullptr;
    VkSemaphore done_ = VK_NULL_HANDLE; bool returned_ = false;
};
float halfToFloat(uint16_t h) {
    const uint32_t sign = (h & 0x8000u) << 16;
    uint32_t exp = (h >> 10) & 31, mantissa = h & 1023, bits;
    if (exp == 0) {
        if (mantissa == 0) bits = sign;
        else { int e = -14; while (!(mantissa & 1024)) { mantissa <<= 1; --e; }
            bits = sign | static_cast<uint32_t>(e + 127) << 23 | (mantissa & 1023) << 13; }
    } else if (exp == 31) bits = sign | 0x7f800000u | mantissa << 13;
    else bits = sign | (exp + 112) << 23 | mantissa << 13;
    float value; std::memcpy(&value, &bits, sizeof(value)); return value;
}
}
struct RenderedFrame::Storage {
    VulkanGraphContext context; VulkanImageResource resource;
    VkSemaphore completion = VK_NULL_HANDLE; VulkanSyncPoint ready;
    mutable std::mutex mutex; mutable std::vector<VulkanSyncPoint> consumers;
    int64_t position = 0; AVRational timeBase{1, 24}; bool encoded = false;
    ColorOutputRequest outputSelection;
    std::string inputPath;
    bool finishedRec709 = false;
    explicit Storage(VulkanGraphContext ctx) : context(ctx), resource(ctx) {
        VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO}; type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; info.pNext = &type;
        require(vkCreateSemaphore(ctx.device, &info, nullptr, &completion) == VK_SUCCESS, "Cannot create frame semaphore");
    }
    ~Storage() {
        try { wait(context.device, ready); for (auto point : consumers) wait(context.device, point); } catch (...) { synchronizedDeviceWaitIdle(context.device); }
        resource.reset(); vkDestroySemaphore(context.device, completion, nullptr);
    }
    void complete(VulkanSyncPoint submitted = {}) {
        wait(context.device, submitted); resource.setLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO}; signal.semaphore = completion; signal.value = 1;
        require(vkSignalSemaphore(context.device, &signal) == VK_SUCCESS, "Cannot signal frame ready"); ready = {completion, 1};
    }
};
RenderedFrame::operator bool() const {
    return storage_ && storage_->resource.ref().valid();
}
VulkanImageRef RenderedFrame::image() const {
    return storage_ ? storage_->resource.ref(storage_->ready) : VulkanImageRef{};
}
int64_t RenderedFrame::position() const {
    return storage_ ? storage_->position : 0;
}
bool RenderedFrame::encoded() const {
    return storage_ && storage_->encoded;
}
const ColorOutputRequest& RenderedFrame::outputSelectionMetadata() const {
    static const ColorOutputRequest empty;
    return storage_ ? storage_->outputSelection : empty;
}
void RenderedFrame::release(VulkanSyncPoint done) const {
    if (!storage_ || !done.valid()) return;
    std::lock_guard lock(storage_->mutex);
    const auto found = std::find_if(storage_->consumers.begin(), storage_->consumers.end(), [&](auto p) { return p.semaphore == done.semaphore; });
    if (found == storage_->consumers.end()) storage_->consumers.push_back(done); else found->value = std::max(found->value, done.value);
}
struct CompositionRenderer::Impl {
    pl_gpu gpu = nullptr; pl_vulkan vulkan = nullptr; VulkanGraphContext context;
    ID3D11Device* d3dDevice = nullptr; ID3D11DeviceContext* d3dContext = nullptr;
    std::unique_ptr<SoftwareContext> software; std::unique_ptr<RenderEngine> placement;
    std::unique_ptr<color::ColorPipeline> colorPipeline;
    std::unique_ptr<VulkanBlendPass> blendPass;
    // Private, bounded ping-pong storage is never handed to a frame consumer.
    std::array<std::unique_ptr<VulkanImageResource>,2> blendCanvas;
    // Devices/queue are fixed for this initialized renderer. Both import paths
    // use the same RGBA16F pool; source format/metadata are handled per frame.
    using InteropKey = std::pair<int, int>;
    std::map<InteropKey, std::unique_ptr<D3D11VulkanInterop>> interops;
    VulkanGraphDocument graphDocument; std::unique_ptr<VulkanFilterGraph> graph;
    D3D11VulkanInterop& interopFor(InteropKey key) {
        const auto found = interops.find(key);
        if (found != interops.end()) return *found->second;
        auto next = std::make_unique<D3D11VulkanInterop>();
        require(next->init(d3dDevice,d3dContext,context.device,context.physicalDevice,context.queue,context.queueFamilyIndex,key.first,key.second), "Cannot initialize interop");
        // A failed/partial initialization never becomes a reusable cache entry.
        return *interops.emplace(key,std::move(next)).first->second;
    }
    struct GradeCache { std::string key; std::unique_ptr<VulkanFilterGraph> graph; };
    std::map<std::string, GradeCache> gradeGraphs;
    pl_dispatch compositor = nullptr;
    pl_shader_obj geometryFilterLut[2] = {nullptr,nullptr};
    std::thread::id owner;
    std::function<bool()> cancelled;
    bool cancellationObserved = false;
    // LRU signatures, not frame counters: metadata/source/output changes may
    // emit another path, while replay/seek/parameter edits do not spam INFO.
    std::vector<std::string> reportedColorPaths;
    void reportColorPath(const std::string& input, const ColorOutputRequest& output) {
        if (input.empty()) return; // blank frames have no source interpretation
        const auto path = input + " outputDisplay='" + output.display + "' outputView='"
            + output.view + "' outputColorSpace='" + output.colorSpace + "' presentationIntent="
            + (output.intent == ColorOutputIntent::SdrVideoPreview ? "SDR video code-preserving preview" : "colorimetric");
        const auto found = std::find(reportedColorPaths.begin(), reportedColorPaths.end(), path);
        if (found != reportedColorPaths.end()) {
            std::rotate(found, found + 1, reportedColorPaths.end());
            return;
        }
        if (reportedColorPaths.size() == 64) reportedColorPaths.erase(reportedColorPaths.begin());
        reportedColorPaths.push_back(path);
        LOG_INFO("CompositionRenderer color path: {}", path);
    }
    void observeCancellation() {
        if (cancelled && cancelled()) cancellationObserved = true;
    }
    void checkCancellation() {
        observeCancellation();
        require(!cancellationObserved, "Rendering cancelled");
    }
    void waitGpu(VulkanSyncPoint point) {
        if (!point.valid()) return;
        VkSemaphoreWaitInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        info.semaphoreCount = 1; info.pSemaphores = &point.semaphore; info.pValues = &point.value;
        VkResult status;
        do { observeCancellation(); status = vkWaitSemaphores(context.device, &info, 10'000'000); }
        while (status == VK_TIMEOUT);
        require(status == VK_SUCCESS, "Renderer timeline wait failed");
        // Caller returns/destroys wrappers before reporting cancellation.
    }
    void check() const {
        require(gpu && owner == std::this_thread::get_id(), "CompositionRenderer requires its initializing worker");
    }
    RenderedFrame allocate(VkExtent2D extent, GraphImageContract contract, int64_t position, AVRational timeBase) {
        RenderedFrame frame; frame.storage_ = std::make_shared<RenderedFrame::Storage>(context);
        require(frame.storage_->resource.ensure(extent, imageUsage, contract), "Cannot allocate renderer RGBA16F image");
        frame.storage_->position = position; frame.storage_->timeBase = timeBase; return frame;
    }
    void commands(const std::function<void(VkCommandBuffer)>& record) {
        VkCommandPool pool = VK_NULL_HANDLE; VkFence fence = VK_NULL_HANDLE;
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pi.queueFamilyIndex = context.queueFamilyIndex;
        require(vkCreateCommandPool(context.device, &pi, nullptr, &pool) == VK_SUCCESS, "Cannot create command pool");
        try {
            VkCommandBuffer cmd; VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ai.commandPool = pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
            require(vkAllocateCommandBuffers(context.device, &ai, &cmd) == VK_SUCCESS, "Cannot allocate command buffer");
            VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            require(vkBeginCommandBuffer(cmd, &bi) == VK_SUCCESS, "Cannot begin commands"); record(cmd);
            require(vkEndCommandBuffer(cmd) == VK_SUCCESS, "Cannot end commands");
            VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; require(vkCreateFence(context.device, &fi, nullptr, &fence) == VK_SUCCESS, "Cannot create fence");
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
            require(synchronizedQueueSubmit(context.queue, 1, &si, fence) == VK_SUCCESS, "Renderer submit failed");
            VkResult status;
            do { observeCancellation(); status = vkWaitForFences(context.device, 1, &fence, VK_TRUE, 10'000'000); }
            while (status == VK_TIMEOUT);
            require(status == VK_SUCCESS, "Renderer fence failed");
            checkCancellation();
        } catch (...) { if (fence) vkDestroyFence(context.device, fence, nullptr); vkDestroyCommandPool(context.device, pool, nullptr); throw; }
        vkDestroyFence(context.device, fence, nullptr); vkDestroyCommandPool(context.device, pool, nullptr);
    }
    static void barrier(VkCommandBuffer cmd, VulkanImageRef image, VkImageLayout from, VkImageLayout to) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; b.oldLayout = from; b.newLayout = to;
        b.srcAccessMask = from == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT; b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image.image; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    }
    void clear(VulkanImageResource& resource) {
        const auto image = resource.ref();
        commands([&](VkCommandBuffer cmd) {
            barrier(cmd,image,image.layout,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkClearColorValue black{}; black.float32[3] = 1.0f;
            VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            vkCmdClearColorImage(cmd,image.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&black,1,&range);
            barrier(cmd,image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }); resource.setLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    void ensureBlendCanvas(VkExtent2D extent) {
        // Blend submissions drain synchronously before reuse, resize or release.
        for (auto& resource : blendCanvas) {
            if (!resource) resource = std::make_unique<VulkanImageResource>(context);
            require(resource->ensure(extent,imageUsage,kWorkingImageContract), "Cannot allocate blend canvas");
        }
    }
    void draw(const pl_frame& input, RenderedFrame& output, bool fit, double sourceAspect = 0) {
        BorrowedTexture texture(gpu, output.image()); auto target = rgb(texture.get(), output.image().extent.width, output.image().extent.height); pl_frame source = input;
        // Identical linear labels make libplacebo unpack/resize only. OCIO owns all color transforms.
        source.color = target.color;
        if (fit) {
            const double src = sourceAspect > 0 ? sourceAspect : (source.crop.x1-source.crop.x0)/(source.crop.y1-source.crop.y0);
            const double dst = static_cast<double>(output.image().extent.width)/output.image().extent.height;
            if (src > dst) { const float h = static_cast<float>(output.image().extent.width/src); target.crop.y0 = (output.image().extent.height-h)*0.5f; target.crop.y1 = target.crop.y0+h; }
            else { const float w = static_cast<float>(output.image().extent.height*src); target.crop.x0 = (output.image().extent.width-w)*0.5f; target.crop.x1 = target.crop.x0+w; }
        }
        pl_render_params params = pl_render_default_params; params.disable_linear_scaling = true;
        // Keep straight alpha through unpacking and fit. The canvas is blended
        // only after each clip's complete grade DAG has run.
        params.background = PL_CLEAR_SKIP; params.border = PL_CLEAR_COLOR;
        params.background_transparency = 1.0f;
        const bool ok = placement->render(&source, &target, &params);
        const bool returned = texture.close();
        output.storage_->resource.setLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        output.storage_->complete(); checkCancellation();
        require(returned && ok, "libplacebo unpack/placement failed");
    }
    // Preparation shader text is invariant; parameter edits use uniforms.
    // Final geometry sampling and mixing belong to the fixed Vulkan program.
    static pl_shader_va canvasCoordinates(pl_gpu gpu) {
        static const float corners[4][2] = {{0,0},{1,0},{0,1},{1,1}};
        pl_shader_va attribute{};
        attribute.attr.name = "canvas_uv";
        attribute.attr.fmt = pl_find_vertex_fmt(gpu, PL_FMT_FLOAT, 2);
        for (int i = 0; i < 4; ++i) attribute.data[i] = corners[i];
        return attribute;
    }
    RenderedFrame maskedPremultiplied(const RenderedFrame& input, const Clip& clip) {
        auto contract = kWorkingImageContract; contract.alpha = AlphaMode::premultiplied;
        auto output = allocate(input.image().extent, contract, input.position(), input.storage_->timeBase);
        BorrowedTexture source(gpu, input.image()), target(gpu, output.image());
        auto shader = pl_dispatch_begin(compositor);
        pl_sample_src sample{}; sample.tex = source.get(); sample.components = 4;
        const bool sampled = pl_shader_sample_direct(shader, &sample);
        const double w = input.image().extent.width, h = input.image().extent.height;
        const double croppedW = w*(1.0-clip.crop.left-clip.crop.right);
        const double croppedH = h*(1.0-clip.crop.top-clip.crop.bottom);
        const double shortSide = std::min(croppedW,croppedH);
        const float box[4] = {float(w*(clip.crop.left+1.0-clip.crop.right)*0.5),
            float(h*(clip.crop.top+1.0-clip.crop.bottom)*0.5), float(croppedW*0.5),float(croppedH*0.5)};
        const float edge[4] = {float(clip.edgeRounding*shortSide*0.5),float(clip.edgeSoftness*shortSide*0.5),float(w),float(h)};
        pl_shader_var variables[2]{};
        variables[0] = {pl_var_vec4("crop_box"),box,true};
        variables[1] = {pl_var_vec4("crop_edge"),edge,true};
        auto uv = canvasCoordinates(gpu);
        pl_custom_shader mask{}; mask.input = mask.output = PL_SHADER_SIG_COLOR;
        mask.description = "Native source crop and premultiply";
        mask.variables = variables; mask.num_variables = 2;
        mask.vertex_attribs = &uv; mask.num_vertex_attribs = 1;
        // Positive SDF is outside. The Palmier transition feathers inward in
        // ORIGINAL source pixels, before local flips or anisotropic scaling.
        mask.body = R"glsl(
            vec2 q = abs(canvas_uv * crop_edge.zw - crop_box.xy)
                       - crop_box.zw + vec2(crop_edge.x);
            float sdf = length(max(q, vec2(0.0)))
                        + min(max(q.x, q.y), 0.0) - crop_edge.x;
            float coverage = 1.0 - smoothstep(-0.5 - crop_edge.y, 0.5, sdf);
            color.a = clamp(color.a, 0.0, 1.0) * coverage;
            color.rgb = color.a > 0.0 ? color.rgb * color.a : vec3(0.0);
        )glsl";
        const bool modified = pl_shader_custom(shader, &mask);
        pl_dispatch_params params{}; params.shader = &shader; params.target = target.get();
        const bool submitted = sampled && modified && pl_dispatch_finish(compositor, &params);
        if (shader) pl_dispatch_abort(compositor, &shader);
        const bool targetReturned = target.close(), sourceReturned = source.close();
        require(submitted && targetReturned && sourceReturned, "Source geometry mask failed");
        output.storage_->resource.setLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        output.storage_->complete(); checkCancellation(); return output;
    }
    RenderedFrame prefilterAxis(const RenderedFrame& input, VkExtent2D extent, int axis) {
        auto output = allocate(extent,input.image().contract,input.position(),input.storage_->timeBase);
        BorrowedTexture source(gpu,input.image()), target(gpu,output.image());
        auto shader = pl_dispatch_begin(compositor);
        pl_sample_src sample{}; sample.tex = source.get(); sample.components = 4;
        sample.new_w = extent.width; sample.new_h = extent.height;
        // Direct filtering deliberately avoids pl_renderer color conversion,
        // which may clamp/gamut-map independent HDR RGB between its internal
        // unpremultiply/premultiply stages. Here all four premult components
        // receive exactly the same widened, normalized reconstruction kernel.
        pl_sample_filter_params filter{}; filter.filter = pl_filter_lanczos;
        filter.lut = &geometryFilterLut[axis];
        const bool sampled = pl_shader_sample_ortho2(shader,&sample,&filter);
        pl_dispatch_params params{}; params.shader = &shader; params.target = target.get();
        const bool submitted = sampled && pl_dispatch_finish(compositor,&params);
        if (shader) pl_dispatch_abort(compositor,&shader);
        const bool targetReturned = target.close(), sourceReturned = source.close();
        require(submitted && targetReturned && sourceReturned, "Premultiplied geometry prefilter failed");
        output.storage_->resource.setLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        output.storage_->complete(); checkCancellation(); return output;
    }
    RenderedFrame prefilter(const RenderedFrame& input, VkExtent2D extent) {
        auto filtered = input;
        if (extent.width != filtered.image().extent.width)
            filtered = prefilterAxis(filtered,{extent.width,filtered.image().extent.height},0);
        if (extent.height != filtered.image().extent.height)
            filtered = prefilterAxis(filtered,extent,1);
        return filtered;
    }
    bool geometryBlend(const VulkanImageRef& backdrop, VulkanImageResource& target,
                       const RenderedFrame& input, const Clip& clip,
                       double sar, double opacity, BlendMode mode) {
        const auto& t = clip.transform;
        const double canvasW = backdrop.extent.width, canvasH = backdrop.extent.height;
        const double sourceW = input.image().extent.width, sourceH = input.image().extent.height;
        VulkanBlendParameters parameters;
        parameters.mode = mode; parameters.opacity = float(opacity);
        const bool identity = t == Transform{} && clip.crop == Crop{}
            && clip.edgeRounding == 0.0 && clip.edgeSoftness == 0.0;
        if (identity && sourceW == canvasW && sourceH == canvasH && sar == 1.0) {
            blendPass->execute(backdrop,input.image(),target.ref(),parameters);
            target.setLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            checkCancellation(); return true;
        }
        const double aspect = sourceW*sar/sourceH;
        const double fitW = std::min(canvasW,canvasH*aspect);
        const double fitH = std::min(canvasH,canvasW/aspect);
        const double width = fitW*t.width, height = fitH*t.height;
        const double radians = std::remainder(t.rotation,360.0)*3.14159265358979323846/180.0;
        const double c = std::cos(radians), s = std::sin(radians);
        // Cull fully off-canvas images before inverse coefficients can overflow
        // for very small sizes or distant (but valid) finite centers.
        const double centerX = canvasW*t.centerX, centerY = canvasH*t.centerY;
        const double boundX = (std::abs(c)*width+std::abs(s)*height)*0.5;
        const double boundY = (std::abs(s)*width+std::abs(c)*height)*0.5;
        if (centerX+boundX < 0 || centerX-boundX > canvasW ||
            centerY+boundY < 0 || centerY-boundY > canvasH) return false;
        require(std::isfinite(width) && std::isfinite(height) && width > 0 && height > 0,
                "Geometry size exceeds GPU coordinate range");
        const double sx = (t.flipHorizontal ? -1.0 : 1.0)/width;
        const double sy = (t.flipVertical ? -1.0 : 1.0)/height;
        const auto coordinate = [](double v) {
            require(std::isfinite(v) && std::abs(v) <= 1.0e30,
                    "Geometry transform exceeds GPU coordinate range");
            return float(v);
        };
        // Inverse clockwise rotation in top-left/y-down canvas coordinates.
        // Rows operate on normalized canvas coordinates and pivot at the FULL
        // image center; crop never changes this mapping or the fitted size.
        const float rowX[4] = {coordinate(c*canvasW*sx),coordinate(s*canvasH*sx),
            coordinate(0.5-(c*centerX+s*centerY)*sx),0};
        const float rowY[4] = {coordinate(-s*canvasW*sy),coordinate(c*canvasH*sy),
            coordinate(0.5-(-s*centerX+c*centerY)*sy),0};
        auto prepared = maskedPremultiplied(input,clip);
        // Downscale in source axes, before rotation. No canvas-sized or
        // unbounded transformed allocation: each axis is <= native resolution.
        const VkExtent2D filtered{static_cast<uint32_t>(std::clamp(std::ceil(width),1.0,sourceW)),
                                 static_cast<uint32_t>(std::clamp(std::ceil(height),1.0,sourceH))};
        if (filtered.width != input.image().extent.width || filtered.height != input.image().extent.height)
            prepared = prefilter(prepared,filtered);
        parameters.affine = true;
        std::copy(std::begin(rowX),std::end(rowX),parameters.sourceX.begin());
        std::copy(std::begin(rowY),std::end(rowY),parameters.sourceY.begin());
        blendPass->execute(backdrop,prepared.image(),target.ref(),parameters);
        target.setLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        checkCancellation(); return true;
    }
    RenderedFrame copy(const VulkanImageRef& input, int64_t position, AVRational timeBase) {
        waitGpu(input.ready); checkCancellation(); auto frame = allocate(input.extent, input.contract, position, timeBase);
        commands([&](VkCommandBuffer cmd) {
            barrier(cmd,input,input.layout,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL); barrier(cmd,frame.image(),VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkImageCopy region{}; region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; region.extent = {input.extent.width,input.extent.height,1};
            vkCmdCopyImage(cmd,input.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,frame.image().image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);
            barrier(cmd,input,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,input.layout); barrier(cmd,frame.image(),VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }); frame.storage_->complete(); return frame;
    }
    RenderedFrame convert(const RenderedFrame& input, const std::string& src, const std::string& dst, GraphImageContract contract, std::string* error) {
        auto frame = allocate(input.image().extent,contract,input.position(),input.storage_->timeBase);
        auto done = colorPipeline->executeSpace(input.image(),frame.image(),src,dst,error);
        checkCancellation();
        require(done.valid(), error && !error->empty() ? error->c_str() : "OCIO GPU conversion failed"); frame.storage_->complete(done); return frame;
    }
    RenderedFrame runGraph(VulkanFilterGraph& executable, const RenderedFrame& input, const FrameContext& timing) {
        require(executable.input()->setVulkanInput(input.image()), "Cannot set graph input"); require(executable.graph()->run(timing), "Filter graph failed");
        VulkanImageRef result; require(executable.output()->getVulkanOutput(result) && result.valid(), "Cannot get graph output");
        auto owned = copy(result,input.position(),input.storage_->timeBase);
        // copy() completed synchronously. The producer may immediately reuse its own allocation.
        auto returned = result; returned.ready = {}; executable.output()->releaseVulkanOutput(returned); return owned;
    }
};
CompositionRenderer::CompositionRenderer() : impl_(std::make_unique<Impl>()) {}
CompositionRenderer::~CompositionRenderer() { shutdown(); }
bool CompositionRenderer::initialize(pl_gpu gpu, pl_vulkan vk, ID3D11Device* device, ID3D11DeviceContext* context, std::string* error) {
    try {
        require(gpu && vk, "CompositionRenderer needs an external GPU context");
        impl_->gpu = gpu; impl_->vulkan = vk; impl_->owner = std::this_thread::get_id();
        impl_->context.instance = vk->instance; impl_->context.device = vk->device; impl_->context.physicalDevice = vk->phys_device; impl_->context.queueFamilyIndex = vk->queue_graphics.index;
        vkGetDeviceQueue(vk->device,vk->queue_graphics.index,0,&impl_->context.queue); impl_->d3dDevice = device; impl_->d3dContext = context;
        impl_->software = std::make_unique<SoftwareContext>(gpu); impl_->placement = std::make_unique<RenderEngine>(gpu); impl_->colorPipeline = std::make_unique<color::ColorPipeline>(impl_->context);
        impl_->blendPass = std::make_unique<VulkanBlendPass>(impl_->context);
        impl_->compositor = pl_dispatch_create(nullptr, gpu);
        require(impl_->compositor != nullptr, "Cannot create libplacebo compositor"); return true;
    } catch (const std::exception& e) { if (error) *error = e.what(); shutdown(); return false; }
}
void CompositionRenderer::setCancellation(std::function<bool()> cancelled) {
    impl_->check(); impl_->cancelled = std::move(cancelled); impl_->cancellationObserved = false;
    impl_->colorPipeline->setCancellation(impl_->cancelled);
    impl_->blendPass->setCancellation(impl_->cancelled);
}
void CompositionRenderer::shutdown() {
    if (!impl_ || !impl_->gpu) return; impl_->check(); pl_gpu_finish(impl_->gpu); synchronizedQueueWaitIdle(impl_->context.queue);
    for (auto& lut : impl_->geometryFilterLut) pl_shader_obj_destroy(&lut);
    impl_->blendCanvas = {}; impl_->blendPass.reset();
    impl_->reportedColorPaths.clear(); impl_->gradeGraphs.clear(); pl_dispatch_destroy(&impl_->compositor); impl_->graph.reset(); impl_->interops.clear(); impl_->colorPipeline.reset(); impl_->software.reset(); impl_->placement.reset(); impl_->gpu = nullptr; impl_->vulkan = nullptr; impl_->owner = {};
}
RenderedFrame CompositionRenderer::render(const CompositionPlan& plan, int64_t frame, const std::vector<CompositionVideoLayer>& layers, std::string* error) {
    CompositionRenderRequest request; request.plan = &plan; request.layers = layers;
    request.settings = plan.settings; request.targetFrame = frame; request.strict = true;
    return render(request,error);
}
RenderedFrame CompositionRenderer::render(const CompositionPlan& plan, int64_t frame, const std::shared_ptr<VideoFrame>& source, std::string* error) {
    std::vector<CompositionVideoLayer> layers;
    const auto samples = plan.videoSamplesAt(frame);
    if (source && !samples.empty()) {
        const auto& sample = samples.front();
        const auto& track = plan.tracks[sample.trackIndex];
        const auto& clip = track.clips[sample.clipIndex];
        layers.push_back({source,track.id,clip.id,sample.clipLocalFrame,clip.visualSnapshot.opacity});
    }
    return render(plan,frame,layers,error);
}
RenderedFrame CompositionRenderer::render(const CompositionRenderRequest& request, std::string* error) {
    try {
        impl_->check(); impl_->checkCancellation(); if (error) error->clear(); if (request.plan) require(supportedColorSettings(request.plan->colorSettings) && request.plan->colorSettings.configId == color::kConfigId, "Unsupported composition color settings");
        const auto& settings = request.plan ? request.plan->settings : request.settings;
        if (!settings.valid(error)) throw std::runtime_error(error && !error->empty() ? *error : "Invalid renderer settings");
        const AVRational timeBase = settings.timeBase().toAVRational();
        VkExtent2D canvas{static_cast<uint32_t>(settings.width()),static_cast<uint32_t>(settings.height())};
        if (request.sourceMode && request.source) canvas = {static_cast<uint32_t>(request.source->get()->width),static_cast<uint32_t>(request.source->get()->height)};
        auto layers = request.layers;
        // Compatibility for explicit source requests used by existing clients.
        // Normal composition packets carry only layers.
        if (layers.empty() && request.source) {
            CompositionVideoLayer layer{request.source,{},request.clipId,request.clipLocalFrame,1.0};
            if (request.plan && !request.sourceMode) {
                const auto samples = request.plan->videoSamplesAt(request.targetFrame);
                for (const auto& sample : samples) {
                    const auto& track = request.plan->tracks[sample.trackIndex];
                    const auto& clip = track.clips[sample.clipIndex];
                    if (!request.clipId.empty() && request.clipId != clip.id) continue;
                    layer.trackId = track.id; layer.clipId = clip.id;
                    layer.clipLocalFrame = sample.clipLocalFrame; layer.opacity = clip.visualSnapshot.opacity;
                    break;
                }
            }
            layers.push_back(std::move(layer));
        }
        // Validate the plan before active-sample, hidden, blank or opacity
        // filtering. Preview must never silently interpret an unknown mode as
        // Normal. Source preview deliberately ignores timeline blend metadata.
        if (request.plan && !request.sourceMode) {
            for (const auto& track : request.plan->tracks) for (const auto& clip : track.clips) {
                if (!parseBlendMode(clip.visualSnapshot.blendMode))
                    throw std::runtime_error("Unsupported clip appearance blend mode: "
                        + clip.visualSnapshot.blendMode.value_or("<unset>"));
            }
        }
        struct Contribution { CompositionVideoLayer layer; const CompositionClip* clip; BlendMode mode; };
        std::vector<Contribution> contributions;
        const auto active = request.plan ? request.plan->videoSamplesAt(request.targetFrame) : std::vector<CompositionSample>{};
        if (!request.blank) for (auto layer : layers) {
            if (!layer.frame) continue;
            require(std::isfinite(layer.opacity), "Layer opacity must be finite");
            layer.opacity = std::clamp(layer.opacity,0.0,1.0);
            if (layer.opacity == 0.0) continue;
            const CompositionClip* clip = nullptr;
            if (request.plan && !request.sourceMode) {
                for (const auto& sample : active) {
                    const auto& track = request.plan->tracks[sample.trackIndex];
                    const auto& candidate = track.clips[sample.clipIndex];
                    if (!track.hidden && !sample.hidden && track.id == layer.trackId && candidate.id == layer.clipId) { clip = &candidate; break; }
                }
                if (!clip) continue; // hidden, inactive or stale packet contribution
            }
            if (request.plan && request.sourceMode && !layer.clipId.empty()) {
                for (const auto& track : request.plan->tracks)
                    for (const auto& candidate : track.clips)
                        if (candidate.id == layer.clipId) { clip = &candidate; break; }
            }
            if (clip && !request.sourceMode) {
                std::string geometryError;
                const bool validGeometry = validateConstantGeometry(clip->visualSnapshot,&geometryError);
                require(validGeometry,geometryError.empty() ? "Invalid constant geometry" : geometryError.c_str());
                if (request.strict) {
                    const auto& appearance = clip->visualSnapshot;
                    require(appearance.transform.rotationX == 0.0 && appearance.transform.rotationY == 0.0,
                            "Unsupported 3D clip rotation");
                    require(!appearance.opacityTrack && !appearance.positionTrack && !appearance.scaleTrack
                            && !appearance.rotationTrack && !appearance.cropTrack,
                            "Unsupported animated clip geometry or opacity");
                }
            }
            const auto mode = clip && !request.sourceMode
                ? *parseBlendMode(clip->visualSnapshot.blendMode) : BlendMode::Normal;
            contributions.push_back({std::move(layer),clip,mode});
        }
        if (request.plan && !request.sourceMode) {
            const auto rank = [&](const Contribution& contribution) {
                return std::find_if(request.plan->tracks.begin(),request.plan->tracks.end(),[&](const auto& track) {
                    return track.id == contribution.layer.trackId;
                }) - request.plan->tracks.begin();
            };
            std::stable_sort(contributions.begin(),contributions.end(),[&](const auto& a, const auto& b) { return rank(a) < rank(b); });
        }
        // Select the complete hardware set before pruning: another layer may
        // still need an extent encountered earlier in this same render.
        std::set<Impl::InteropKey> activeInterops;
        for (const auto& contribution : contributions) {
            const auto* av = contribution.layer.frame->get();
            if (av->format == AV_PIX_FMT_D3D11) activeInterops.emplace(av->width,av->height);
        }
        std::erase_if(impl_->interops,[&](const auto& entry) { return !activeInterops.contains(entry.first); });
        std::erase_if(impl_->gradeGraphs,[&](const auto& entry) {
            return std::none_of(contributions.begin(),contributions.end(),[&](const auto& contribution) {
                return contribution.clip && contribution.clip->id == entry.first;
            });
        });
        const auto& baseRequest = request;
        auto prepare = [&](const Contribution& contribution) {
        auto request = baseRequest;
        request.source = contribution.layer.frame;
        request.clipId = contribution.layer.clipId;
        request.clipLocalFrame = contribution.layer.clipLocalFrame;
        const auto* clip = contribution.clip;
        std::string overrideSpace = request.inputColorSpace;
        if (overrideSpace.empty() && clip && request.plan) { const auto found = request.plan->colorSettings.inputColorSpaces.find(clip->mediaRef); if (found != request.plan->colorSettings.inputColorSpaces.end()) overrideSpace = found->second; }
        const int inputPolicyVersion = request.plan ? request.plan->colorSettings.inputPolicyVersion : 2;
        const auto selection = impl_->colorPipeline->resolveInput(request.source->spec(),overrideSpace,inputPolicyVersion);
        if (request.diagnostics) request.diagnostics->insert(request.diagnostics->end(),selection.diagnostics.begin(),selection.diagnostics.end());
        const auto* av = request.source->get();
        auto encodedContract = kWorkingImageContract; encodedContract.primaries = filtergraph::ColorPrimaries::unknown; encodedContract.transfer = filtergraph::ColorTransfer::unknown; RenderedFrame working;
        if (av->format == AV_PIX_FMT_D3D11) {
            require(impl_->d3dDevice && impl_->d3dContext, "D3D11 source needs interop device");
            require(av->data[0] != nullptr, "D3D11 source needs a texture");
            const Impl::InteropKey key{av->width,av->height};
            auto& interop = impl_->interopFor(key);
            D3D11_TEXTURE2D_DESC desc{}; reinterpret_cast<ID3D11Texture2D*>(av->data[0])->GetDesc(&desc); VulkanImageRef imported;
            const bool importedOk = desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT
                ? interop.processRgbFrame(av,imported) : interop.processFrame(av,imported);
            if (!importedOk) {
                // Processing may have acquired a slot without exporting it.
                // Retire that pool rather than reusing partially acquired state.
                impl_->interops.erase(key);
                require(false, "Cannot import D3D11 frame");
            }
            struct ImportedFrameReturn {
                D3D11VulkanInterop& interop;
                VulkanSyncPoint done;
                ~ImportedFrameReturn() { interop.releaseFrame(done); }
            } returned{interop,imported.ready};
            working = impl_->allocate(imported.extent,kWorkingImageContract,request.targetFrame,timeBase);
            auto done = impl_->colorPipeline->executeSpace(imported,working.image(),selection.colorSpace,color::kWorkingSpace,error);
            if (done.valid()) returned.done = done;
            require(done.valid(), error && !error->empty() ? error->c_str() : "OCIO IDT failed"); working.storage_->complete(done);
        } else {
            auto unpacked = impl_->allocate({static_cast<uint32_t>(av->width),static_cast<uint32_t>(av->height)},encodedContract,request.targetFrame,timeBase);
            auto source = impl_->software->uploadAvFrame(av); require(source != nullptr, "Cannot upload source frame"); impl_->draw(*source,unpacked,false); working = impl_->convert(unpacked,selection.colorSpace,color::kWorkingSpace,kWorkingImageContract,error);
        }
        FrameContext timing; timing.pts = request.targetFrame; timing.frameIndex = request.clipLocalFrame; timing.timeBaseNum = timeBase.num; timing.timeBaseDen = timeBase.den; timing.timeSeconds = request.clipLocalFrame*av_q2d(timeBase);
        if (clip) {
            grade::CompiledGrade compiled;
            const bool compiledOk = grade::GradeCompiler::compile(clip->visualSnapshot,request.clipLocalFrame,compiled,error,{request.strict},impl_->colorPipeline.get(), &request.plan->gradeLuts);
            require(compiledOk, error && !error->empty() ? error->c_str() : "Grade compile failed");
            if (request.diagnostics) for (const auto& diagnostic : compiled.diagnostics) request.diagnostics->push_back(diagnostic.message);
            if (!compiled.neutral()) {
                impl_->checkCancellation();
                auto& cache = impl_->gradeGraphs[clip->id];
                if (!cache.graph || cache.key != compiled.topologyKey) {
                    cache.graph = std::make_unique<VulkanFilterGraph>(impl_->context,compiled.document);
                    cache.key = compiled.topologyKey;
                } else {
                    for (const auto& patch : compiled.parameterUpdates)
                        require(cache.graph->setParameters(patch.nodeId,patch.parameters), "Cannot update Grade parameters");
                    for (const auto& patch : compiled.programUpdates)
                        require(cache.graph->setProgram(patch.nodeId,patch.program), "Cannot update Grade GPU program");
                }
                working = impl_->runGraph(*cache.graph,working,timing);
            }
        }
        const auto& metadata = selection.observedMetadata;
        const auto* formatName = av_get_pix_fmt_name(metadata.format);
        const auto repr = colorReprFromAvFrame(av);
        std::ostringstream path;
        const auto sourceId = !request.sourceId.empty() ? request.sourceId
            : clip ? (!clip->mediaRef.empty() ? clip->mediaRef : clip->sourcePath) : "<anonymous>";
        path << "source='" << sourceId << "' size=" << av->width << 'x' << av->height
             << " pri=" << int(metadata.colorPrimaries) << " trc=" << int(metadata.colorTransfer)
             << " matrix=" << int(metadata.colorSpace) << " range=" << int(metadata.colorRange)
             << " format=" << int(metadata.format) << '(' << (formatName ? formatName : "unknown") << ')'
             << " chroma=" << int(metadata.chromaLocation);
        if (av->format == AV_PIX_FMT_D3D11) {
            D3D11_TEXTURE2D_DESC desc{};
            reinterpret_cast<ID3D11Texture2D*>(av->data[0])->GetDesc(&desc);
            path << " dxgiFormat=" << int(desc.Format);
        }
        path << " effectiveRange=" << (repr.levels == PL_COLOR_LEVELS_FULL ? "full" : "limited");
        if (metadata.colorRange == AVCOL_RANGE_UNSPECIFIED)
            path << (repr.levels == PL_COLOR_LEVELS_FULL ? " (unspecified; RGB adapter uses full)"
                                                       : " (unspecified; adapter fallback limited)");
        path << " effectiveMatrix=" << int(repr.sys) << " interpretedInput='" << selection.colorSpace
             << "' selectionOrigin=" << (selection.source == color::InputSelectionSource::Override ? "override"
                 : selection.source == color::InputSelectionSource::Metadata ? "metadata" : "fallback")
             << " policy=" << inputPolicyVersion << " working='" << color::kWorkingSpace << "'";
        working.storage_->inputPath = path.str();
        working.storage_->finishedRec709 = inputPolicyVersion == 2 && selection.colorSpace == color::kExportDisplay;
        return working;
        };
        impl_->ensureBlendCanvas(canvas);
        size_t currentCanvas = 0;
        impl_->clear(*impl_->blendCanvas[currentCanvas]);
        bool finishedRec709 = !contributions.empty();
        std::string inputPath;
        if (!contributions.empty()) {
            pl_dispatch_reset_frame(impl_->compositor);
            // UI tracks and request layers are top first; composite bottom to
            // top into private ping-pong storage, never into an input image.
            for (auto it = contributions.rbegin(); it != contributions.rend(); ++it) {
                impl_->checkCancellation(); auto layer = prepare(*it);
                finishedRec709 = finishedRec709 && layer.storage_->finishedRec709;
                if (!inputPath.empty()) inputPath += " | ";
                inputPath += layer.storage_->inputPath;
                const auto* av = it->layer.frame->get();
                const double sar = av->sample_aspect_ratio.num > 0 && av->sample_aspect_ratio.den > 0
                    ? av_q2d(av->sample_aspect_ratio) : 1.0;
                const Clip appearance = !request.sourceMode && it->clip ? it->clip->visualSnapshot : Clip{};
                const size_t nextCanvas = 1-currentCanvas;
                if (impl_->geometryBlend(impl_->blendCanvas[currentCanvas]->ref(),
                        *impl_->blendCanvas[nextCanvas],layer,appearance,sar,it->layer.opacity,it->mode))
                    currentCanvas = nextCanvas;
            }
        }
        // The consumer owns an independent allocation. Future renders, resize
        // and shutdown can recycle/free both scratch images without touching it.
        auto composite = impl_->copy(impl_->blendCanvas[currentCanvas]->ref(),request.targetFrame,timeBase);
        composite.storage_->inputPath = std::move(inputPath);
        composite.storage_->finishedRec709 = finishedRec709;
        if (impl_->graph) {
            FrameContext timing; timing.pts = request.targetFrame;
            timing.frameIndex = request.sourceMode ? request.clipLocalFrame : request.targetFrame;
            timing.timeBaseNum = timeBase.num; timing.timeBaseDen = timeBase.den;
            timing.timeSeconds = timing.frameIndex*av_q2d(timeBase);
            auto filtered = impl_->runGraph(*impl_->graph,composite,timing);
            filtered.storage_->inputPath = composite.storage_->inputPath;
            filtered.storage_->finishedRec709 = composite.storage_->finishedRec709;
            composite = std::move(filtered);
        }
        return composite;
    } catch (const std::exception& e) { if (error) *error = e.what(); return {}; }
}
RenderedFrame CompositionRenderer::output(const RenderedFrame& input, const ColorOutputRequest& request, std::string* error) {
    try {
        impl_->check(); impl_->checkCancellation(); require(input && !input.encoded() && input.image().contract == kWorkingImageContract, "Output requires ACEScg image");
        auto contract = kWorkingImageContract; contract.primaries = filtergraph::ColorPrimaries::unknown; contract.transfer = filtergraph::ColorTransfer::unknown;
        auto frame = impl_->allocate(input.image().extent,contract,input.position(),input.storage_->timeBase);
        ColorOutputRequest resolved;
        if (request.colorSpace.empty()) {
            resolved.display = request.display.empty() ? color::kPreviewDisplay : request.display;
            resolved.view = request.view.empty() ? color::kOutputView : request.view;
        } else resolved.colorSpace = request.colorSpace;
        if (request.intent == ColorOutputIntent::SdrVideoPreview && input.storage_->finishedRec709 &&
            resolved.colorSpace.empty() && resolved.display == color::kPreviewDisplay && resolved.view == color::kColorimetricView) {
            // Reverse the finished-video input encoding rather than changing
            // its SDR code values through a different display transfer curve.
            resolved.display = color::kExportDisplay;
            resolved.intent = ColorOutputIntent::SdrVideoPreview;
        }
        auto done = resolved.colorSpace.empty() ? impl_->colorPipeline->executeDisplay(input.image(),frame.image(),color::kWorkingSpace,resolved.display,resolved.view,error)
            : impl_->colorPipeline->executeSpace(input.image(),frame.image(),color::kWorkingSpace,resolved.colorSpace,error);
        require(done.valid(), error && !error->empty() ? error->c_str() : "OCIO output failed");
        frame.storage_->complete(done); frame.storage_->encoded = true;
        frame.storage_->outputSelection = std::move(resolved);
        frame.storage_->inputPath = input.storage_->inputPath;
        impl_->reportColorPath(frame.storage_->inputPath,frame.storage_->outputSelection);
        return frame;
    } catch (const std::exception& e) { if (error) *error = e.what(); return {}; }
}
std::shared_ptr<AVFrame> CompositionRenderer::download(const RenderedFrame& input, DownloadFormat format, std::string* error) {
    VkBuffer buffer = VK_NULL_HANDLE; VkDeviceMemory memory = VK_NULL_HANDLE;
    try {
        impl_->check(); impl_->checkCancellation(); require(static_cast<bool>(input), "Download needs owned frame"); impl_->waitGpu(input.image().ready); impl_->checkCancellation(); const auto image = input.image(); const VkDeviceSize bytes = static_cast<VkDeviceSize>(image.extent.width)*image.extent.height*8;
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = bytes; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT; require(vkCreateBuffer(impl_->context.device,&bi,nullptr,&buffer) == VK_SUCCESS, "Cannot allocate readback buffer");
        VkMemoryRequirements req; vkGetBufferMemoryRequirements(impl_->context.device,buffer,&req); VkPhysicalDeviceMemoryProperties props; vkGetPhysicalDeviceMemoryProperties(impl_->context.physicalDevice,&props);
        uint32_t type = UINT32_MAX; for (uint32_t i = 0; i < props.memoryTypeCount; ++i) if ((req.memoryTypeBits & (1u<<i)) && (props.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { type = i; break; }
        require(type != UINT32_MAX, "No coherent readback memory"); VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = req.size; ai.memoryTypeIndex = type;
        require(vkAllocateMemory(impl_->context.device,&ai,nullptr,&memory) == VK_SUCCESS && vkBindBufferMemory(impl_->context.device,buffer,memory,0) == VK_SUCCESS, "Cannot bind readback memory");
        impl_->commands([&](VkCommandBuffer cmd) { Impl::barrier(cmd,image,image.layout,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL); VkBufferImageCopy region{}; region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; region.imageExtent = {image.extent.width,image.extent.height,1}; vkCmdCopyImageToBuffer(cmd,image.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer,1,&region); Impl::barrier(cmd,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,image.layout); });
        AVFrame* raw = av_frame_alloc(); require(raw != nullptr, "Cannot allocate download frame"); auto frame = std::shared_ptr<AVFrame>(raw,[](AVFrame* p) { av_frame_free(&p); });
        raw->width = image.extent.width; raw->height = image.extent.height; raw->format = format == DownloadFormat::RgbaFloat ? AV_PIX_FMT_RGBAF32 : AV_PIX_FMT_RGBA64; raw->pts = input.position(); raw->duration = 1; raw->time_base = input.storage_->timeBase; raw->sample_aspect_ratio = {1,1}; raw->color_range = AVCOL_RANGE_JPEG; raw->colorspace = AVCOL_SPC_RGB;
        raw->color_primaries = AVCOL_PRI_UNSPECIFIED; raw->color_trc = input.encoded() ? AVCOL_TRC_UNSPECIFIED : AVCOL_TRC_LINEAR;
        require(av_frame_get_buffer(raw,32) >= 0, "Cannot allocate download pixels"); void* mapped; require(vkMapMemory(impl_->context.device,memory,0,bytes,0,&mapped) == VK_SUCCESS, "Cannot map readback");
        const auto* halves = static_cast<const uint16_t*>(mapped);
        for (uint32_t y = 0; y < image.extent.height; ++y) for (uint32_t x = 0; x < image.extent.width*4; ++x) { const float value = halfToFloat(halves[static_cast<size_t>(y)*image.extent.width*4+x]);
            if (format == DownloadFormat::RgbaFloat) reinterpret_cast<float*>(raw->data[0]+y*raw->linesize[0])[x] = value;
            else reinterpret_cast<uint16_t*>(raw->data[0]+y*raw->linesize[0])[x] = static_cast<uint16_t>(std::lround(std::clamp(std::isfinite(value) ? value : 0.0f,0.0f,1.0f)*65535.0f)); }
        vkUnmapMemory(impl_->context.device,memory); vkDestroyBuffer(impl_->context.device,buffer,nullptr); vkFreeMemory(impl_->context.device,memory,nullptr); return frame;
    } catch (const std::exception& e) { if (buffer) vkDestroyBuffer(impl_->context.device,buffer,nullptr); if (memory) vkFreeMemory(impl_->context.device,memory,nullptr); if (error) *error = e.what(); return {}; }
}
bool CompositionRenderer::setFilterGraphSnapshot(const VulkanGraphDocument& document, std::string* error) {
    try { impl_->check(); std::unique_ptr<VulkanFilterGraph> next; if (!document.nodes().empty()) next = std::make_unique<VulkanFilterGraph>(impl_->context,document); impl_->graph = std::move(next); impl_->graphDocument = document; return true; }
    catch (const std::exception& e) { if (error) *error = e.what(); return false; }
}
VulkanGraphDocument CompositionRenderer::filterGraphSnapshot() const {
    return impl_->graphDocument;
}
bool CompositionRenderer::setNodeParameter(uint64_t id, const std::string& name, float value) {
    impl_->check(); auto next = impl_->graphDocument; if (!impl_->graph || !next.updateParameter(id,{{name,value}})) return false;
    for (const auto& node : next.nodes()) if (node.id == id) { if (!impl_->graph->setParameters(id,node.parameter)) return false; impl_->graphDocument = std::move(next); return true; } return false;
}
} // namespace heisenberg::renderer
