#include "VideoPresenter.hpp"
#include "SwapChain.hpp"
#include "RenderEngine.hpp"
#include <Utiles/Logger.hpp>
#include <utility>

namespace heisenberg::renderer {
namespace {
bool fail(std::string* error, const char* message) {
    if (error) *error = message;
    LOG_ERROR("VideoPresenter: {}", message);
    return false;
}

bool outputColorSpace(const ColorOutputRequest& selection, pl_color_space& color) {
    // These are resolved OCIO output names, not the input/working-space tags.
    // A view may tone-map, but the display still determines its final encoding.
    const auto& space = selection.colorSpace.empty() ? selection.display : selection.colorSpace;
    color = pl_color_space_srgb;
    if (space == "sRGB - Display" || space == "sRGB Encoded Rec.709 (sRGB)") return true;
    if (space == "Rec.1886 Rec.709 - Display") {
        // The fixed studio config's Rec.1886 display is a pure 2.4 encoding.
        if (selection.intent != ColorOutputIntent::SdrVideoPreview ||
            selection.view != "Un-tone-mapped" || !selection.colorSpace.empty())
            color.transfer = PL_COLOR_TRC_GAMMA24;
        // The explicit video-preview intent presents these SDR code values
        // unchanged, matching unmanaged SDR playback on the sRGB surface.
        return true;
    }
    return false;
}

bool supportedTarget(pl_gpu gpu, const pl_swapchain_frame& target, VkFormat& format) {
    if (!target.fbo || !target.fbo->params.renderable) return false;
    if (!pl_vulkan_unwrap(gpu, target.fbo, &format, nullptr)) return false;
    // Inspect the actual VkImage format, not pl_fmt.name. libplacebo 365 has no
    // implicit-sRGB pl_fmt contract. Reject *_SRGB rather than sending encoded
    // values to an implicit linear-write attachment (which would double encode).
    switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        break;
    default:
        return false;
    }
    const auto& repr = target.color_repr;
    const auto& space = target.color_space;
    return repr.sys == PL_COLOR_SYSTEM_RGB && repr.levels == PL_COLOR_LEVELS_FULL &&
           !repr.dovi && repr.bits.bit_shift == 0 &&
           (!repr.bits.sample_depth || !repr.bits.color_depth || repr.bits.sample_depth == repr.bits.color_depth) &&
           space.primaries == PL_COLOR_PRIM_BT_709 && space.transfer == PL_COLOR_TRC_SRGB &&
           !pl_color_space_is_hdr(&space);
}

pl_frame rgbFrame(pl_tex texture, const pl_color_space& color) {
    pl_frame frame{};
    frame.num_planes = 1;
    frame.planes[0].texture = texture;
    frame.planes[0].components = 4;
    for (int i = 0; i < 4; ++i) frame.planes[0].component_mapping[i] = i;
    frame.repr = pl_color_repr_rgb;
    frame.repr.alpha = PL_ALPHA_INDEPENDENT;
    frame.color = color;
    frame.crop = {0, 0, static_cast<float>(texture->params.w), static_cast<float>(texture->params.h)};
    return frame;
}
}

bool blitPresentationFrame(pl_gpu gpu, RenderEngine& blitter, pl_tex texture,
                           const ColorOutputRequest& selection,
                           const pl_swapchain_frame& surface, std::string* error) {
    if (error) error->clear();
    if (!gpu || !texture || !texture->params.sampleable || texture->params.w <= 0 || texture->params.h <= 0)
        return fail(error, "invalid presentation source");
    pl_color_space sourceColor{};
    if (!outputColorSpace(selection, sourceColor))
        return fail(error, "unsupported/unknown encoded output selection; expected resolved sRGB or Rec.1886 Rec.709 output");
    VkFormat format = VK_FORMAT_UNDEFINED;
    if (!supportedTarget(gpu, surface, format))
        return fail(error, "unsupported surface contract; expected full-range BT.709 sRGB UNORM (no HDR/scRGB or implicit-sRGB attachment)");

    auto source = rgbFrame(texture, sourceColor);
    pl_frame target{};
    // Includes surface repr, color, physical extent, component mapping and
    // plane orientation. Do not replace these with requested window dimensions.
    pl_frame_from_swapchain(&target, &surface);
    const float width = static_cast<float>(surface.fbo->params.w);
    const float height = static_cast<float>(surface.fbo->params.h);
    const float aspect = static_cast<float>(texture->params.w) / texture->params.h;
    target.crop = {0, 0, width, height};
    if (aspect > width / height) {
        const float h = width / aspect;
        target.crop.y0 = (height - h) * 0.5f;
        target.crop.y1 = target.crop.y0 + h;
    } else {
        const float w = height * aspect;
        target.crop.x0 = (width - w) * 0.5f;
        target.crop.x1 = target.crop.x0 + w;
    }
    pl_render_params params = pl_render_default_params;
    params.disable_linear_scaling = true;
    // Colorimetric Rec.1886 is adapted once to sRGB. Explicit SDR video preview
    // instead preserves code values; neither path reruns an ACES rendering view.
    if (!blitter.render(&source, &target, &params)) return fail(error, "final presentation blit failed");
    return true;
}

struct VideoPresenter::Impl {
    pl_gpu gpu = nullptr; pl_vulkan vulkan = nullptr;
    std::unique_ptr<SwapChain> swapChain;
    std::unique_ptr<RenderEngine> blitter;
    ResizeCallback onResize; PresentCallback onPresent;
    int width = 0, height = 0;
    VkSemaphore readDone = VK_NULL_HANDLE;
    uint64_t readValue = 0;
    bool loggedSurface = false;
};
VideoPresenter::VideoPresenter() : impl_(std::make_unique<Impl>()) {}
VideoPresenter::~VideoPresenter() { shutdown(); }
bool VideoPresenter::initialize(pl_gpu gpu, pl_vulkan vk, std::unique_ptr<SwapChain> swapChain, int width, int height) {
    if (!gpu || !vk || !swapChain || !swapChain->isValid()) return false;
    impl_->gpu = gpu; impl_->vulkan = vk; impl_->swapChain = std::move(swapChain);
    impl_->width = width; impl_->height = height;
    impl_->loggedSurface = false;
    pl_vulkan_sem_params semaphore{}; semaphore.type = VK_SEMAPHORE_TYPE_TIMELINE;
    impl_->readDone = pl_vulkan_sem_create(gpu, &semaphore);
    if (!impl_->readDone) { shutdown(); return false; }
    impl_->blitter = std::make_unique<RenderEngine>(gpu); return true;
}
void VideoPresenter::shutdown() {
    if (!impl_->gpu) return;
    pl_gpu_finish(impl_->gpu);
    impl_->blitter.reset(); impl_->swapChain.reset();
    if (impl_->readDone) pl_vulkan_sem_destroy(impl_->gpu, &impl_->readDone);
    impl_->gpu = nullptr; impl_->vulkan = nullptr;
}
void VideoPresenter::resize(int width, int height) {
    impl_->width = width; impl_->height = height;
    if (impl_->swapChain) impl_->swapChain->resize(&width,&height);
    if (impl_->onResize) impl_->onResize(width,height);
}
void VideoPresenter::setOnResize(ResizeCallback callback) { impl_->onResize = std::move(callback); }
void VideoPresenter::setOnPresent(PresentCallback callback) { impl_->onPresent = std::move(callback); }
bool VideoPresenter::presentFrame(const RenderedFrame& frame) {
    if (!impl_->gpu || !impl_->swapChain || !frame || !frame.encoded() || impl_->width <= 0 || impl_->height <= 0) return false;
    pl_color_space sourceColor{};
    if (!outputColorSpace(frame.outputSelectionMetadata(), sourceColor))
        return fail(nullptr, "unsupported/unknown RenderedFrame output encoding");
    const auto image = frame.image();
    pl_vulkan_wrap_params wp{}; wp.image = image.image; wp.width = image.extent.width; wp.height = image.extent.height; wp.format = image.vkFormat; wp.usage = image.usage;
    auto sourceTexture = pl_vulkan_wrap(impl_->gpu,&wp); if (!sourceTexture) return false;
    pl_vulkan_release_params release{}; release.tex = sourceTexture; release.layout = image.layout; release.qf = image.queueFamilyIndex;
    release.semaphore.sem = image.ready.semaphore; release.semaphore.value = image.ready.value;
    pl_vulkan_release_ex(impl_->gpu,&release);
    pl_swapchain_frame surface{};
    const bool acquired = impl_->swapChain->startFrame(impl_->width, impl_->height, surface);
    bool success = acquired;
    if (acquired) {
        if (!impl_->loggedSurface && surface.fbo) {
            VkFormat format = VK_FORMAT_UNDEFINED;
            pl_vulkan_unwrap(impl_->gpu, surface.fbo, &format, nullptr);
            const auto plFormat = surface.fbo->params.format;
            LOG_INFO("VideoPresenter: actual surface VkFormat={} ({}) plformat={} extent={}x{} repr(sys={},levels={},alpha={},sample_depth={},color_depth={},shift={}) primaries={} transfer={} flipped={}",
                     static_cast<int>(format), vk::to_string(static_cast<vk::Format>(format)), plFormat ? plFormat->name : "unknown",
                     surface.fbo->params.w, surface.fbo->params.h,
                     static_cast<int>(surface.color_repr.sys), static_cast<int>(surface.color_repr.levels), static_cast<int>(surface.color_repr.alpha),
                     surface.color_repr.bits.sample_depth, surface.color_repr.bits.color_depth, surface.color_repr.bits.bit_shift,
                     pl_color_primaries_name(surface.color_space.primaries), pl_color_transfer_name(surface.color_space.transfer), surface.flipped);
            impl_->loggedSurface = true;
        }
        success = blitPresentationFrame(impl_->gpu, *impl_->blitter, sourceTexture, frame.outputSelectionMetadata(), surface);
        if (!success && surface.fbo) {
            // There is no swapchain cancel API. Consume the acquired frame in
            // lockstep, but never expose unknown/incorrect encoded image data.
            const float black[4] = {0, 0, 0, 1};
            pl_tex_clear(impl_->gpu, surface.fbo, black);
        }
    }
    // Synchronous final read: the lease may be released immediately after this
    // call and no borrowed swapchain/graph resource escapes to another frame.
    pl_gpu_flush(impl_->gpu);
    pl_vulkan_hold_params hold{}; hold.tex = sourceTexture; hold.layout = image.layout; hold.qf = image.queueFamilyIndex;
    hold.semaphore.sem = impl_->readDone; hold.semaphore.value = ++impl_->readValue;
    const bool returned = pl_vulkan_hold_ex(impl_->gpu,&hold);
    pl_gpu_finish(impl_->gpu); pl_tex_destroy(impl_->gpu,&sourceTexture);
    // Even failure must unlock an acquired swapchain frame.
    const bool submitted = acquired && impl_->swapChain->submitFrame();
    if (!success || !returned || !submitted) return false;
    impl_->swapChain->swapBuffers(); if (impl_->onPresent) impl_->onPresent(); return true;
}
} // namespace heisenberg::renderer
