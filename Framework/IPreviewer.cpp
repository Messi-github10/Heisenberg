#include "IPreviewer.hpp"

#include "Controller/PlaybackController.hpp"
#include "Platform/D3D11/D3D11Context.hpp"
#include "Platform/GpuContext.hpp"
#include "Platform/Vulkan/VulkanContext.hpp"
#include "MultiMedia/Video/Renderer/FilterGraph/Interface/INodeFactory.hpp"
#include "MultiMedia/Video/Renderer/FilterGraph/Vulkan/Graph/VulkanFilterGraph.hpp"
#include "MultiMedia/Video/Renderer/SwapChain.hpp"
#include "MultiMedia/Video/Renderer/VideoPresenter.hpp"

#include <Utiles/Logger.hpp>

#include <cstdint>
#include <stdexcept>
#include <utility>
#include <variant>

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
        attachedGraph_ = nullptr;
        if (!timelineGraph_ && !pendingTimelineGraph_.empty()) {
            loadPlaylistFilters();
        }
        applyGraphForFrame(lastFrame_.get());

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
        attachedGraph_ = nullptr;
        videoWidth_ = 0;
        videoHeight_ = 0;
    }

    void resize(int w, int h) override {
        if (!presenter_) return;
        presenter_->resize(w, h);
        presentCurrentFrame();
    }

    void open(const std::string& path) override {
        clearTimelineGraph();
        if (playback_) playback_->open(path);
    }

    void openPlaylist(const std::string& path) override {
        clearTimelineGraph();
        if (!playback_) return;
        playback_->openPlaylist(path);
        const auto& filters = playback_->playlistFilters();
        if (!filters.empty()) {
            pendingTimelineGraph_ = filters.front().graph;
            if (filters.size() > 1) {
                LOG_WARN("IPreviewer: v1 applies only the first Playlist filter");
            }
            loadPlaylistFilters();
        }
    }

    void close() override {
        if (playback_) playback_->close();
        clearTimelineGraph();
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

    bool setNodeParameter(uint64_t nodeId,
                          const std::string& name,
                          float value) override {
        if (!filterGraph_) return false;
        filtergraph::VulkanGraphParameter patch;
        patch[name] = value;
        if (!graphDocument_.updateParameter(nodeId, patch)) return false;
        const auto* node = findDocumentNode(nodeId);
        if (!node || !filterGraph_->setParameters(nodeId, node->parameter)) {
            return false;
        }
        presentCurrentFrame();
        return true;
    }

    bool setFilterParameter(const std::string& filterId,
                            const std::string& name,
                            float value) override {
        const uint64_t nodeId = findNodeByFilterId(filterId);
        return nodeId != 0 && setNodeParameter(nodeId, name, value);
    }

    bool getFilterParameter(const std::string& filterId,
                            const std::string& name,
                            float& value) const override {
        const auto* node = findDocumentNode(findNodeByFilterId(filterId));
        if (!node) return false;
        const auto found = node->parameter.find(name);
        if (found == node->parameter.end()) return false;
        if (const auto* number = std::get_if<float>(&found->second)) {
            value = *number;
            return true;
        }
        if (const auto* number = std::get_if<int32_t>(&found->second)) {
            value = static_cast<float>(*number);
            return true;
        }
        return false;
    }

    uint64_t findNodeByFilterId(const std::string& filterId) const override {
        for (const auto& node : graphDocument_.nodes()) {
            if (node.filterId == filterId) return node.id;
        }
        return 0;
    }

    void shutdown() override {
        if (shutdownDone_) return;
        shutdownDone_ = true;

        listener_ = nullptr;
        if (playback_) playback_->close();
        detachWindow();
        clearTimelineGraph();
        filterGraph_.reset();
        graphDocument_ = {};
        timelineGraph_.reset();
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
            if (!timelineGraph_ && !pendingTimelineGraph_.empty()) {
                loadPlaylistFilters();
            }
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

        presentCurrentFrame();

        filtergraph::VulkanFilterGraph* active = attachedGraph_;
        if (!active || (++filterGraphVerificationFrame_ % 60) != 0) return;

        filtergraph::VulkanImageRef output;
        if (!active->output()->getVulkanOutput(output)) {
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
        attachedGraph_ = nullptr;
        filterGraph_.reset();
        filterGraph_ = std::move(nextGraph);
        graphDocument_ = std::move(graphDocument);
        filterGraphVerificationFrame_ = 0;
        filterGraphPath_ = path;
        if (listener_) listener_->onFilterGraphChanged(path);
        presentCurrentFrame();
        return true;
    }

    void loadPlaylistFilters() {
        if (!playback_) return;
        const auto& filters = playback_->playlistFilters();
        if (filters.empty()) return;
        if (filters.size() > 1) {
            LOG_WARN("IPreviewer: v1 applies only the first Playlist filter");
        }
        std::string error;
        if (!loadTimelineGraph(filters.front().graph, &error)) {
            LOG_ERROR("IPreviewer: playlist filter graph load failed: {}", error);
            if (listener_) listener_->onFilterGraphFailed(error);
            return;
        }
        LOG_INFO("IPreviewer: loaded playlist filter graph '{}' in={} out={}",
                 filters.front().graph, filters.front().in, filters.front().out);
    }

    bool loadTimelineGraph(const std::string& path, std::string* error) {
        if (path.empty()) {
            if (error) *error = "Filter graph path is empty";
            return false;
        }
        if (!gpuCtx_) {
            pendingTimelineGraph_ = path;
            return true;
        }
        if (!presenter_) {
            pendingTimelineGraph_ = path;
            return true;
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
        attachedGraph_ = nullptr;
        timelineGraph_.reset();
        timelineGraph_ = std::move(nextGraph);
        timelineDocument_ = std::move(graphDocument);
        timelineGraphPath_ = path;
        pendingTimelineGraph_.clear();
        return true;
    }

    void clearTimelineGraph() {
        if (presenter_ && attachedGraph_ && attachedGraph_ == timelineGraph_.get()) {
            presenter_->setFilterGraph(nullptr, nullptr, nullptr);
        }
        attachedGraph_ = nullptr;
        timelineGraph_.reset();
        timelineDocument_ = {};
        timelineGraphPath_.clear();
        pendingTimelineGraph_.clear();
        timelineGraphEnabled_ = false;
    }

    filtergraph::VulkanFilterGraph* graphForFrame(const AVFrame* frame) {
        if (timelineGraph_ && playback_ && frame) {
            const auto& filters = playback_->playlistFilters();
            if (!filters.empty() && filters.front().covers(frame->pts)) {
                return timelineGraph_.get();
            }
        }
        if (filterGraph_) return filterGraph_.get();
        return nullptr;
    }

    void applyGraphForFrame(const AVFrame* frame) {
        if (!presenter_) return;
        if (!timelineGraph_ && !pendingTimelineGraph_.empty()) {
            std::string error;
            loadTimelineGraph(pendingTimelineGraph_, &error);
        }
        filtergraph::VulkanFilterGraph* wanted = graphForFrame(frame);
        if (wanted == attachedGraph_) return;
        if (wanted) {
            presenter_->setFilterGraph(wanted->graph(), wanted->input(),
                                       wanted->output());
        } else {
            presenter_->setFilterGraph(nullptr, nullptr, nullptr);
        }
        const bool enabled = wanted == timelineGraph_.get() && wanted != nullptr;
        if (enabled != timelineGraphEnabled_) {
            timelineGraphEnabled_ = enabled;
            LOG_INFO("IPreviewer: playlist filter {} at frame {}",
                     enabled ? "on" : "off",
                     frame ? frame->pts : -1);
        }
        attachedGraph_ = wanted;
    }

    void presentCurrentFrame() {
        if (!presenter_ || !lastFrame_ || !lastFrame_->data[0]) return;
        applyGraphForFrame(lastFrame_.get());
        if (!presenter_->presentFrame(lastFrame_.get())) {
            LOG_WARN("IPreviewer: presentFrame failed (format={}, {}x{})",
                     lastFrame_->format, lastFrame_->width, lastFrame_->height);
        }
    }

    const filtergraph::VulkanGraphNodeDesc* findDocumentNode(
        uint64_t nodeId) const {
        for (const auto& node : graphDocument_.nodes()) {
            if (node.id == nodeId) return &node;
        }
        return nullptr;
    }

    TaskDispatcher dispatcher_;
    Listener* listener_ = nullptr;
    std::unique_ptr<ctrl::PlaybackController> playback_;
    std::unique_ptr<renderer::GpuContext> gpuCtx_;
    std::unique_ptr<renderer::VideoPresenter> presenter_;
    std::unique_ptr<filtergraph::VulkanFilterGraph> filterGraph_;
    filtergraph::VulkanGraphDocument graphDocument_;
    std::unique_ptr<filtergraph::VulkanFilterGraph> timelineGraph_;
    filtergraph::VulkanGraphDocument timelineDocument_;
    filtergraph::VulkanFilterGraph* attachedGraph_ = nullptr;
    std::shared_ptr<AVFrame> lastFrame_;
    std::string filterGraphPath_;
    std::string timelineGraphPath_;
    std::string pendingTimelineGraph_;
    bool timelineGraphEnabled_ = false;
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
