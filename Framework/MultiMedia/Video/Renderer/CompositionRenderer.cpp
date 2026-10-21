#include "CompositionRenderer.hpp"
#include "Color/ColorPipeline.hpp"
#include "ColorSpaceUtils.hpp"
#include "RenderEngine.hpp"
#include <Compositing/CompositionPlan.hpp>
#include <Compositing/Grade/GradeCompiler.hpp>
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
}
#include <d3d11.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>

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
    std::unique_ptr<color::ColorPipeline> colorPipeline; std::unique_ptr<D3D11VulkanInterop> interop;
    int interopW = 0, interopH = 0; VulkanGraphDocument graphDocument; std::unique_ptr<VulkanFilterGraph> graph;
    struct GradeCache { std::string key; std::unique_ptr<VulkanFilterGraph> graph; };
    GradeCache gradeGraph; std::thread::id owner;
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
    void hold(pl_tex texture, RenderedFrame& frame) {
        pl_gpu_flush(gpu); pl_vulkan_hold_params params{}; params.tex = texture; params.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; params.qf = context.queueFamilyIndex;
        params.semaphore.sem = frame.storage_->completion; params.semaphore.value = 1;
        require(pl_vulkan_hold_ex(gpu, &params), "Cannot return libplacebo ownership");
        frame.storage_->ready = {frame.storage_->completion, 1}; waitGpu(frame.storage_->ready); frame.storage_->resource.setLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    void draw(const pl_frame& input, RenderedFrame& output, bool fit, double sourceAspect = 0) {
        auto texture = wrap(gpu, output.image()); auto target = rgb(texture, output.image().extent.width, output.image().extent.height); pl_frame source = input;
        // Identical linear labels make libplacebo unpack/resize only. OCIO owns all color transforms.
        source.color = target.color;
        if (fit) {
            const double src = sourceAspect > 0 ? sourceAspect : (source.crop.x1-source.crop.x0)/(source.crop.y1-source.crop.y0);
            const double dst = static_cast<double>(output.image().extent.width)/output.image().extent.height;
            if (src > dst) { const float h = static_cast<float>(output.image().extent.width/src); target.crop.y0 = (output.image().extent.height-h)*0.5f; target.crop.y1 = target.crop.y0+h; }
            else { const float w = static_cast<float>(output.image().extent.height*src); target.crop.x0 = (output.image().extent.width-w)*0.5f; target.crop.x1 = target.crop.x0+w; }
        }
        pl_render_params params = pl_render_default_params; params.disable_linear_scaling = true;
        const bool ok = placement->render(&source, &target, &params); hold(texture, output); pl_tex_destroy(gpu, &texture); checkCancellation(); require(ok, "libplacebo unpack/placement failed");
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
        impl_->software = std::make_unique<SoftwareContext>(gpu); impl_->placement = std::make_unique<RenderEngine>(gpu); impl_->colorPipeline = std::make_unique<color::ColorPipeline>(impl_->context); return true;
    } catch (const std::exception& e) { if (error) *error = e.what(); shutdown(); return false; }
}
void CompositionRenderer::setCancellation(std::function<bool()> cancelled) {
    impl_->check(); impl_->cancelled = std::move(cancelled); impl_->cancellationObserved = false;
    impl_->colorPipeline->setCancellation(impl_->cancelled);
}
void CompositionRenderer::shutdown() {
    if (!impl_ || !impl_->gpu) return; impl_->check(); pl_gpu_finish(impl_->gpu); synchronizedQueueWaitIdle(impl_->context.queue);
    impl_->reportedColorPaths.clear(); impl_->gradeGraph = {}; impl_->graph.reset(); impl_->interop.reset(); impl_->colorPipeline.reset(); impl_->software.reset(); impl_->placement.reset(); impl_->gpu = nullptr; impl_->vulkan = nullptr; impl_->owner = {};
}
RenderedFrame CompositionRenderer::render(const CompositionPlan& plan, int64_t frame, const std::shared_ptr<VideoFrame>& source, std::string* error) {
    CompositionRenderRequest request; request.plan = &plan; request.source = source; request.settings = plan.settings; request.targetFrame = frame; request.strict = true; request.blank = !source || plan.hidden;
    if (auto sample = plan.videoSampleAt(frame)) { request.clipLocalFrame = sample->clipLocalFrame; request.clipId = plan.clips[sample->clipIndex].id; request.blank = request.blank || sample->hidden; }
    else request.blank = true;
    return render(request,error);
}
RenderedFrame CompositionRenderer::render(const CompositionRenderRequest& request, std::string* error) {
    try {
        impl_->check(); impl_->checkCancellation(); if (error) error->clear(); if (request.plan) require(supportedColorSettings(request.plan->colorSettings) && request.plan->colorSettings.configId == color::kConfigId, "Unsupported composition color settings");
        const auto& settings = request.plan ? request.plan->settings : request.settings;
        if (!settings.valid(error)) throw std::runtime_error(error && !error->empty() ? *error : "Invalid renderer settings");
        const AVRational timeBase = settings.timeBase().toAVRational();
        const CompositionClip* clip = nullptr; if (request.plan) for (const auto& candidate : request.plan->clips) if (candidate.id == request.clipId) { clip = &candidate; break; }
        VkExtent2D canvas{static_cast<uint32_t>(settings.width()),static_cast<uint32_t>(settings.height())};
        if (request.sourceMode && request.source) canvas = {static_cast<uint32_t>(request.source->get()->width),static_cast<uint32_t>(request.source->get()->height)};
        if (request.blank || !request.source || (request.plan && request.plan->hidden)) {
            auto frame = impl_->allocate(canvas,kWorkingImageContract,request.targetFrame,timeBase);
            impl_->commands([&](VkCommandBuffer cmd) {
                Impl::barrier(cmd,frame.image(),VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL); VkClearColorValue black{}; black.float32[3] = 1.0f; VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
                vkCmdClearColorImage(cmd,frame.image().image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&black,1,&range); Impl::barrier(cmd,frame.image(),VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }); frame.storage_->complete(); return frame;
        }
        std::string overrideSpace = request.inputColorSpace;
        if (overrideSpace.empty() && clip && request.plan) { const auto found = request.plan->colorSettings.inputColorSpaces.find(clip->mediaRef); if (found != request.plan->colorSettings.inputColorSpaces.end()) overrideSpace = found->second; }
        const int inputPolicyVersion = request.plan ? request.plan->colorSettings.inputPolicyVersion : 2;
        const auto selection = impl_->colorPipeline->resolveInput(request.source->spec(),overrideSpace,inputPolicyVersion);
        if (request.diagnostics) request.diagnostics->insert(request.diagnostics->end(),selection.diagnostics.begin(),selection.diagnostics.end());
        const auto* av = request.source->get();
        auto encodedContract = kWorkingImageContract; encodedContract.primaries = filtergraph::ColorPrimaries::unknown; encodedContract.transfer = filtergraph::ColorTransfer::unknown; RenderedFrame working;
        if (av->format == AV_PIX_FMT_D3D11) {
            require(impl_->d3dDevice && impl_->d3dContext, "D3D11 source needs interop device");
            if (!impl_->interop || impl_->interopW != av->width || impl_->interopH != av->height) {
                impl_->interop = std::make_unique<D3D11VulkanInterop>(); require(impl_->interop->init(impl_->d3dDevice,impl_->d3dContext,impl_->context.device,impl_->context.physicalDevice,impl_->context.queue,impl_->context.queueFamilyIndex,av->width,av->height), "Cannot initialize interop"); impl_->interopW = av->width; impl_->interopH = av->height;
            }
            D3D11_TEXTURE2D_DESC desc{}; reinterpret_cast<ID3D11Texture2D*>(av->data[0])->GetDesc(&desc); VulkanImageRef imported;
            require(desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? impl_->interop->processRgbFrame(av,imported) : impl_->interop->processFrame(av,imported), "Cannot import D3D11 frame");
            working = impl_->allocate(imported.extent,kWorkingImageContract,request.targetFrame,timeBase);
            auto done = impl_->colorPipeline->executeSpace(imported,working.image(),selection.colorSpace,color::kWorkingSpace,error); impl_->interop->releaseFrame(done.valid() ? done : imported.ready);
            require(done.valid(), error && !error->empty() ? error->c_str() : "OCIO IDT failed"); working.storage_->complete(done);
        } else {
            auto unpacked = impl_->allocate({static_cast<uint32_t>(av->width),static_cast<uint32_t>(av->height)},encodedContract,request.targetFrame,timeBase);
            auto source = impl_->software->uploadAvFrame(av); require(source != nullptr, "Cannot upload source frame"); impl_->draw(*source,unpacked,false); working = impl_->convert(unpacked,selection.colorSpace,color::kWorkingSpace,kWorkingImageContract,error);
        }
        if (working.image().extent.width != canvas.width || working.image().extent.height != canvas.height || av->sample_aspect_ratio.num != av->sample_aspect_ratio.den) {
            auto placed = impl_->allocate(canvas,kWorkingImageContract,request.targetFrame,timeBase); auto texture = wrap(impl_->gpu,working.image()); auto source = rgb(texture,av->width,av->height);
            const double sar = av->sample_aspect_ratio.num > 0 && av->sample_aspect_ratio.den > 0 ? av_q2d(av->sample_aspect_ratio) : 1.0; impl_->draw(source,placed,true,av->width*sar/av->height);
            pl_vulkan_hold_params hold{}; hold.tex = texture; hold.layout = working.image().layout; hold.qf = impl_->context.queueFamilyIndex;
            hold.semaphore.sem = placed.image().ready.semaphore; hold.semaphore.value = 2;
            const bool returned = pl_vulkan_hold_ex(impl_->gpu,&hold);
            pl_gpu_finish(impl_->gpu); pl_tex_destroy(impl_->gpu,&texture);
            require(returned, "Cannot release placement input"); working = std::move(placed);
        }
        FrameContext timing; timing.pts = request.targetFrame; timing.frameIndex = request.clipLocalFrame; timing.timeBaseNum = timeBase.num; timing.timeBaseDen = timeBase.den; timing.timeSeconds = request.clipLocalFrame*av_q2d(timeBase);
        if (clip) {
            grade::CompiledGrade compiled;
            const bool compiledOk = grade::GradeCompiler::compile(clip->visualSnapshot,request.clipLocalFrame,compiled,error,{request.strict},impl_->colorPipeline.get(), &request.plan->gradeLuts);
            require(compiledOk, error && !error->empty() ? error->c_str() : "Grade compile failed");
            if (request.diagnostics) for (const auto& diagnostic : compiled.diagnostics) request.diagnostics->push_back(diagnostic.message);
            if (!compiled.neutral()) {
                impl_->checkCancellation();
                auto& cache = impl_->gradeGraph;
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
        if (impl_->graph) working = impl_->runGraph(*impl_->graph,working,timing);
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
