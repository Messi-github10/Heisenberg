#include "IPreviewer.hpp"

#include "Controller/PlaybackController.hpp"
#include "Platform/D3D11/D3D11Context.hpp"
#include "Platform/GpuContext.hpp"
#include "Platform/Vulkan/VulkanContext.hpp"
#include "Video/Renderer/FilterGraph/Interface/INodeFactory.hpp"
#include "Video/Renderer/FilterGraph/Vulkan/Graph/VulkanFilterGraph.hpp"
#include "Video/Renderer/SwapChain.hpp"
#include "Video/Renderer/VideoPresenter.hpp"

#include <Utiles/Logger.hpp>

#include <stdexcept>
#include <utility>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace heisenberg {
namespace {

IPreviewer::State toPreviewerState(ctrl::PlaybackController::State state) {
    switch (state) {
        case ctrl::PlaybackController::Idle:      return IPreviewer::State::Idle;
        case ctrl::PlaybackController::Loading:   return IPreviewer::State::Loading;
        case ctrl::PlaybackController::Playing:   return IPreviewer::State::Playing;
        case ctrl::PlaybackController::Paused:    return IPreviewer::State::Paused;
        case ctrl::PlaybackController::Scrubbing: return IPreviewer::State::Scrubbing;
        case ctrl::PlaybackController::Ended:     return IPreviewer::State::Ended;
    }
    return IPreviewer::State::Idle;
}

class Previewer final : public IPreviewer {
public:
    Previewer() {
        playback_ = std::make_unique<ctrl::PlaybackController>();
        bindPlayback();
    }

    ~Previewer() override {
        shutdown();
    }

    void setTaskDispatcher(TaskDispatcher dispatcher) override {
        dispatcher_ = std::move(dispatcher);
        if (playback_) playback_->setTaskDispatcher(dispatcher_);
    }

    void setListener(Listener* listener) override {
        listener_ = listener;
    }

    void attachWindow(void* nativeSurface, int w, int h) override {
        if (!nativeSurface) {
            LOG_ERROR("IPreviewer: native surface is null");
            return;
        }
        if (!ensureGpu() || !gpuCtx_ || !playback_) return;

        detachWindow();

        const int width = w > 0 ? w : 640;
        const int height = h > 0 ? h : 360;

        auto& vkCtx = renderer::VulkanContext::instance();
        auto swapChain = std::make_unique<renderer::SwapChain>();
        if (!swapChain->initialize(gpuCtx_->plVulkan(), vkCtx.vkInstance(),
                                   nativeSurface, width, height)) {
            LOG_ERROR("IPreviewer: SwapChain initialization failed");
            return;
        }

        presenter_ = std::make_unique<renderer::VideoPresenter>();
        if (!presenter_->initialize(gpuCtx_->plGpu(), gpuCtx_->plVulkan(),
                                    std::move(swapChain), width, height)) {
            LOG_ERROR("IPreviewer: VideoPresenter initialization failed");
            presenter_.reset();
            return;
        }

        presenter_->setD3D11Device(
            renderer::D3D11Context::instance().device(),
            renderer::D3D11Context::instance().context());
        presenter_->setOnResize([this](int width, int height) {
            videoWidth_ = width;
            videoHeight_ = height;
        });
        if (filterGraph_) {
            presenter_->setFilterGraph(filterGraph_->graph(),
                                       filterGraph_->input(),
                                       filterGraph_->output());
        }

        nativeSurface_ = nativeSurface;
        videoWidth_ = width;
        videoHeight_ = height;
        LOG_INFO("IPreviewer: attached native surface 0x{:x} {}x{}",
                 reinterpret_cast<uintptr_t>(nativeSurface), width, height);
    }

    void detachWindow() override {
        if (presenter_) {
            presenter_->setFilterGraph(nullptr, nullptr, nullptr);
            presenter_->shutdown();
            presenter_.reset();
        }
        nativeSurface_ = nullptr;
        lastFrame_.reset();
        videoWidth_ = 0;
        videoHeight_ = 0;
    }

    void resize(int w, int h) override {
        if (!presenter_) return;
        presenter_->resize(w, h);
        if (lastFrame_) presenter_->presentFrame(lastFrame_.get());
    }

    void open(const std::string& path) override {
        if (playback_) playback_->open(path);
    }

    void close() override {
        if (playback_) playback_->close();
        lastFrame_.reset();
        videoWidth_ = 0;
        videoHeight_ = 0;
    }

    void play() override {
        if (playback_) playback_->play();
    }

    void pause() override {
        if (playback_) playback_->pause();
    }

    void togglePlayPause() override {
        if (playback_) playback_->togglePlayPause();
    }

    void seek(double seconds) override {
        if (playback_) playback_->seek(seconds);
    }

    void beginScrub() override {
        if (playback_) playback_->beginScrub();
    }

    void scrubToFrame(int64_t frameIndex) override {
        if (playback_) playback_->scrubToFrame(frameIndex);
    }

    void endScrub(int64_t frameIndex) override {
        if (playback_) playback_->endScrub(frameIndex);
    }

    void stepForward(int frames) override {
        if (playback_) playback_->stepForward(frames);
    }

    void stepBackward(int frames) override {
        if (playback_) playback_->stepBackward(frames);
    }

    void goToStart() override {
        if (playback_) playback_->goToStart();
    }

    void goToEnd() override {
        if (playback_) playback_->goToEnd();
    }

    void setHardwareDecode(bool enabled) override {
        if (playback_) playback_->setHardwareDecode(enabled);
    }

    void openFilterGraph(const std::string& path) override {
        std::string error;
        if (!loadFilterGraph(path, &error)) {
            LOG_ERROR("IPreviewer: filter graph load failed: {}", error);
            if (listener_) listener_->onFilterGraphFailed(error);
            return;
        }
        LOG_INFO("IPreviewer: loaded filter graph '{}'", path);
    }

    void shutdown() override {
        if (shutdownDone_) return;
        shutdownDone_ = true;

        listener_ = nullptr;
        if (playback_) playback_->close();
        detachWindow();
        filterGraph_.reset();
        gpuCtx_.reset();
        LOG_INFO("IPreviewer: GPU resources released");
    }

    State state() const override {
        return playback_ ? toPreviewerState(playback_->state()) : State::Idle;
    }

    bool isPlaying() const override {
        return playback_ && playback_->isPlaying();
    }

    double currentTime() const override {
        return playback_ ? playback_->currentTime() : 0.0;
    }

    double duration() const override {
        return playback_ ? playback_->duration() : 0.0;
    }

    bool isSeekable() const override {
        return playback_ && playback_->isSeekable();
    }

    double fps() const override {
        return playback_ ? playback_->fps() : 0.0;
    }

    int64_t frameCount() const override {
        return playback_ ? playback_->frameCount() : 0;
    }

    const std::string& filterGraphPath() const override {
        return filterGraphPath_;
    }

private:
    bool ensureGpu() {
        if (gpuCtx_) return true;
        try {
            auto& vkCtx = renderer::VulkanContext::instance();
            vkCtx.initLoader();
            vkCtx.createInstance();
            vkCtx.createDevice();
            renderer::D3D11Context::instance().createDevice();

            renderer::VulkanResources vkRes;
            vkRes.instance = vkCtx.vkInstance();
            vkRes.physDevice = vkCtx.physicalDevice();
            vkRes.device = vkCtx.device();
            vkRes.graphicsQF = vkCtx.graphicsQueueFamily();
            vkRes.graphicsQueue = vkCtx.graphicsQueue();
            vkRes.getProcAddr = vkCtx.getInstanceProcAddr();
            gpuCtx_ = std::make_unique<renderer::GpuContext>(vkRes);
            return true;
        } catch (const std::exception& error) {
            LOG_ERROR("IPreviewer: GPU initialization failed — {}", error.what());
            gpuCtx_.reset();
            return false;
        }
    }

    void bindPlayback() {
        playback_->setTaskDispatcher(dispatcher_);
        playback_->onStateChanged = [this](ctrl::PlaybackController::State state) {
            if (listener_) listener_->onStateChanged(toPreviewerState(state));
        };
        playback_->onFrameDecoded = [this](std::shared_ptr<AVFrame> frame) {
            presentDecodedFrame(std::move(frame));
        };
        playback_->onPositionChanged = [this](double seconds) {
            if (listener_) listener_->onPositionChanged(seconds);
        };
        playback_->onDurationChanged = [this](double seconds) {
            if (listener_) listener_->onDurationChanged(seconds);
        };
        playback_->onEndOfStream = [this] {
            if (listener_) listener_->onEndOfStream();
        };
        playback_->onOpenFailed = [this](const std::string& reason) {
            if (listener_) listener_->onOpenFailed(reason);
        };
    }

    void presentDecodedFrame(std::shared_ptr<AVFrame> frame) {
        if (!frame || !frame->data[0] || !presenter_) return;

        lastFrame_ = frame;
        if (videoWidth_ <= 0 || videoHeight_ <= 0) {
            videoWidth_ = frame->width;
            videoHeight_ = frame->height;
        }

        const bool hardware = frame->format == AV_PIX_FMT_D3D11;
        if (frame->pts != AV_NOPTS_VALUE && frame->time_base.num > 0
            && frame->time_base.den > 0) {
            const int64_t ptsUs = av_rescale_q(
                frame->pts, frame->time_base, AVRational{1, 1'000'000});
            LOG_DEBUG("IPreviewer: decoded frame mode={} pts={} ptsUs={} "
                      "timeBase={}/{} format={} size={}x{} range={} colorspace={} "
                      "primaries={} transfer={} chromaLocation={}",
                      hardware ? "hardware" : "software", frame->pts, ptsUs,
                      frame->time_base.num, frame->time_base.den, frame->format,
                      frame->width, frame->height,
                      static_cast<int>(frame->color_range),
                      static_cast<int>(frame->colorspace),
                      static_cast<int>(frame->color_primaries),
                      static_cast<int>(frame->color_trc),
                      static_cast<int>(frame->chroma_location));
        }

        if (!presenter_->presentFrame(frame.get())) {
            LOG_WARN("IPreviewer: presentFrame failed (format={}, {}x{})",
                     frame->format, frame->width, frame->height);
            return;
        }

        if (!filterGraph_ || (++filterGraphVerificationFrame_ % 60) != 0) return;

        filtergraph::VulkanImageRef output;
        if (!filterGraph_->output()->getVulkanOutput(output)) {
            LOG_WARN("FilterGraph verify: output is unavailable");
            return;
        }
        LOG_INFO("FilterGraph verify: output={}x{} is ready",
                 output.extent.width, output.extent.height);
    }

    bool loadFilterGraph(const std::string& path, std::string* error) {
        if (path.empty()) {
            if (error) *error = "Filter graph path is empty";
            return false;
        }
        if (!presenter_ || !gpuCtx_) {
            if (error) *error = "Vulkan previewer is not initialized";
            return false;
        }

        auto& vkCtx = renderer::VulkanContext::instance();
        filtergraph::VulkanGraphContext graphContext;
        graphContext.instance = static_cast<VkInstance>(vkCtx.vkInstance());
        graphContext.physicalDevice =
            static_cast<VkPhysicalDevice>(vkCtx.physicalDevice());
        graphContext.device = static_cast<VkDevice>(vkCtx.device());
        graphContext.queue = static_cast<VkQueue>(vkCtx.graphicsQueue());
        graphContext.queueFamilyIndex = vkCtx.graphicsQueueFamily();

        filtergraph::VulkanGraphDocument graphDocument;
        std::string graphError;
        if (!graphDocument.loadFromJsonFile(path, &graphError)) {
            if (error) *error = graphError;
            return false;
        }

        std::unique_ptr<filtergraph::VulkanFilterGraph> nextGraph;
        try {
            nextGraph = std::make_unique<filtergraph::VulkanFilterGraph>(
                graphContext, graphDocument);
        } catch (const std::exception& exception) {
            if (error) *error = exception.what();
            return false;
        }

        vkCtx.device().waitIdle();
        presenter_->setFilterGraph(nullptr, nullptr, nullptr);
        filterGraph_.reset();
        filterGraph_ = std::move(nextGraph);
        presenter_->setFilterGraph(filterGraph_->graph(), filterGraph_->input(),
                                   filterGraph_->output());

        filterGraphVerificationFrame_ = 0;
        filterGraphPath_ = path;
        if (listener_) listener_->onFilterGraphChanged(path);
        if (lastFrame_) presenter_->presentFrame(lastFrame_.get());
        return true;
    }

    TaskDispatcher dispatcher_;
    Listener* listener_ = nullptr;
    std::unique_ptr<ctrl::PlaybackController> playback_;
    std::unique_ptr<renderer::GpuContext> gpuCtx_;
    std::unique_ptr<renderer::VideoPresenter> presenter_;
    std::unique_ptr<filtergraph::VulkanFilterGraph> filterGraph_;
    std::shared_ptr<AVFrame> lastFrame_;
    std::string filterGraphPath_;
    void* nativeSurface_ = nullptr;
    int videoWidth_ = 0;
    int videoHeight_ = 0;
    uint64_t filterGraphVerificationFrame_ = 0;
    bool shutdownDone_ = false;
};

} // namespace

void IPreviewer::initLoader() {
    renderer::VulkanContext::instance().initLoader();
}

std::unique_ptr<IPreviewer> IPreviewer::create() {
    return std::make_unique<Previewer>();
}

} // namespace heisenberg
