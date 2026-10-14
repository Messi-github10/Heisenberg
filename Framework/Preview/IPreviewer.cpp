#include "IPreviewer.hpp"

#include "VideoEngine.hpp"
#include <Compositing/FrameRenderer.hpp>

#include <atomic>
#include <mutex>
#include "MultiMedia/Video/Renderer/RenderThread.hpp"
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

class Previewer final : public IPreviewer {
public:
    Previewer() {
        playback_ = std::make_unique<VideoEngine>();
        bindPlayback();
        render_.start([this] {
            if (!playback_) return 2;
            int waitMs = 2;
            const auto pump = playback_->pumpFrame(waitMs);
            if (pump == VideoEngine::Pump::Paused
                || pump == VideoEngine::Pump::Ended) {
                return -1;
            }
            return waitMs;
        });
    }

    ~Previewer() override {
        shutdown();
    }

    void setTaskDispatcher(TaskDispatcher dispatcher) override {
        std::lock_guard lock(events_->mutex);
        events_->dispatcher = std::move(dispatcher);
    }

    void setListener(Listener* listener) override {
        events_->listener.store(listener, std::memory_order_release);
    }

    void attachWindow(void* nativeSurface, int w, int h) override {
        if (!nativeSurface) {
            LOG_ERROR("IPreviewer: native surface is null");
            return;
        }
        const int width = w > 0 ? w : 640;
        const int height = h > 0 ? h : 360;
        render_.post([this, nativeSurface, width, height] {
            attachWindowOnRenderThread(nativeSurface, width, height);
        });
    }

    void detachWindow() override {
        render_.postSync([this] { detachWindowOnRenderThread(); });
    }

    void resize(int w, int h) override {
        render_.post([this, w, h] {
            if (!presenter_) return;
            presenter_->resize(w, h);
            presentCurrentFrame();
        });
    }

    void open(const std::string& path) override {
        if (playback_) playback_->open(path);
        resetPresentation();
    }

    void openPlaylist(const std::string& path) override {
        if (playback_) playback_->openPlaylist(path);
        resetPresentation();
    }

    void openTimeline(const Timeline& timeline, const MediaManifest& manifest,
                      const std::string& projectDir) override {
        if (playback_) playback_->openTimeline(timeline, manifest, projectDir);
        resetPresentation();
    }

    void close() override {
        if (playback_) playback_->close();
        resetPresentation();
    }

    void play() override {
        if (playback_) {
            playback_->play();
            events_->generation.store(playback_->snapshot().generation, std::memory_order_release);
        }
        render_.setPlaying(playback_ && playback_->snapshot().state == State::Playing);
    }

    void pause() override {
        if (playback_) playback_->pause();
        render_.setPlaying(false);
    }

    void togglePlayPause() override {
        if (playback_) {
            playback_->togglePlayPause();
            events_->generation.store(playback_->snapshot().generation, std::memory_order_release);
        }
        render_.setPlaying(playback_ && playback_->snapshot().state == State::Playing);
    }

    void seek(double seconds) override {
        if (playback_) {
            playback_->seek(seconds);
            events_->generation.store(playback_->snapshot().generation, std::memory_order_release);
        }
    }

    void beginScrub() override {
        if (playback_) playback_->beginScrub();
    }

    void scrubToFrame(int64_t frameIndex) override {
        if (playback_) {
            playback_->scrubToFrame(frameIndex);
            events_->generation.store(playback_->snapshot().generation, std::memory_order_release);
        }
    }

    void endScrub(int64_t frameIndex) override {
        if (playback_) {
            playback_->endScrub(frameIndex);
            events_->generation.store(playback_->snapshot().generation, std::memory_order_release);
        }
    }

    void stepForward(int frames) override {
        if (playback_) {
            playback_->stepForward(frames);
            events_->generation.store(playback_->snapshot().generation, std::memory_order_release);
        }
    }

    void stepBackward(int frames) override {
        if (playback_) {
            playback_->stepBackward(frames);
            events_->generation.store(playback_->snapshot().generation, std::memory_order_release);
        }
    }

    void goToStart() override {
        if (playback_) {
            playback_->goToStart();
            events_->generation.store(playback_->snapshot().generation, std::memory_order_release);
        }
    }

    void goToEnd() override {
        if (playback_) {
            playback_->goToEnd();
            events_->generation.store(playback_->snapshot().generation, std::memory_order_release);
        }
    }

    void setHardwareDecode(bool enabled) override {
        if (playback_) playback_->setHardwareDecode(enabled);
    }

    void openFilterGraph(const std::string& path) override {
        render_.post([this, path] {
            std::string error;
            if (!loadFilterGraph(path, &error)) {
                LOG_ERROR("IPreviewer: filter graph load failed: {}", error);
                notifyListener([error](Listener& listener) { listener.onFilterGraphFailed(error); });
                return;
            }
            LOG_INFO("IPreviewer: loaded filter graph '{}'", path);
        });
    }

    bool setNodeParameter(uint64_t nodeId,
                          const std::string& name,
                          float value) override {
        bool success = false;
        render_.postSync([this, nodeId, name, value, &success] {
            if (!filterGraph_) return;
            filtergraph::VulkanGraphParameter patch;
            patch[name] = value;
            if (!graphDocument_.updateParameter(nodeId, patch)) return;
            const auto* node = findDocumentNode(nodeId);
            if (!node) return;
            success = filterGraph_->setParameters(nodeId, node->parameter);
            if (success) presentCurrentFrame();
        });
        return success;
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
        bool success = false;
        render_.postSync([this, &success, &value, filterId, name] {
            for (const auto& node : graphDocument_.nodes()) {
                if (node.filterId != filterId) continue;
                const auto found = node.parameter.find(name);
                if (found == node.parameter.end()) return;
                if (const auto* number = std::get_if<float>(&found->second)) { value = *number; success = true; }
                else if (const auto* number = std::get_if<int32_t>(&found->second)) { value = static_cast<float>(*number); success = true; }
                return;
            }
        });
        return success;
    }

    uint64_t findNodeByFilterId(const std::string& filterId) const override {
        uint64_t result = 0;
        render_.postSync([this, filterId, &result] {
            for (const auto& node : graphDocument_.nodes())
                if (node.filterId == filterId) { result = node.id; return; }
        });
        return result;
    }

    void shutdown() override {
        if (shutdownDone_) return;
        shutdownDone_ = true;

        events_->alive.store(false, std::memory_order_release);
        events_->listener.store(nullptr, std::memory_order_release);
        if (playback_) playback_->shutdown();
        render_.postSync([this] {
            detachWindowOnRenderThread();
            filterGraph_.reset();
            graphDocument_ = {};
            if (frameRenderer_) { frameRenderer_->shutdown(); frameRenderer_.reset(); }
            lastPacket_.reset();
            gpuCtx_.reset();
        });
        render_.stop();
        LOG_INFO("IPreviewer: GPU resources released");
    }

    State state() const override {
        return playback_ ? playback_->snapshot().state : State::Idle;
    }

    bool isPlaying() const override {
        return playback_ && playback_->snapshot().state == State::Playing;
    }

    double currentTime() const override {
        return playback_ ? playback_->snapshot().currentTime : 0.0;
    }

    double duration() const override {
        return playback_ ? playback_->snapshot().duration : 0.0;
    }

    bool isSeekable() const override {
        return playback_ && playback_->snapshot().seekable;
    }

    double fps() const override {
        return playback_ ? playback_->snapshot().fps : 0.0;
    }

    int64_t frameCount() const override {
        return playback_ ? playback_->snapshot().frameCount : 0;
    }

    std::string filterGraphPath() const override {
        std::string result;
        render_.postSync([this, &result] { result = filterGraphPath_; });
        return result;
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

    struct Events {
        std::mutex mutex;
        TaskDispatcher dispatcher;
        std::atomic<Listener*> listener{nullptr};
        std::atomic<bool> alive{true};
        std::atomic<uint64_t> generation{0};
    };
    template<class F> void notifyListener(F callback, uint64_t generation = 0) {
        auto events = events_;
        TaskDispatcher dispatcher;
        { std::lock_guard lock(events->mutex); dispatcher = events->dispatcher; }
        auto task = [events, generation, callback = std::move(callback)] {
            if (!events->alive.load(std::memory_order_acquire)
                || (generation != 0 && events->generation.load(std::memory_order_acquire) != generation)) return;
            if (auto* listener = events->listener.load(std::memory_order_acquire)) callback(*listener);
        };
        if (dispatcher) dispatcher(std::move(task)); else task();
    }

    template<class F> void forwardListener(F callback) {
        if (!events_->alive.load(std::memory_order_acquire)) return;
        if (auto* listener = events_->listener.load(std::memory_order_acquire)) callback(*listener);
    }

    void bindPlayback() {
        const auto events = events_;
        playback_->setTaskDispatcher([events](Task task) {
            TaskDispatcher dispatcher;
            { std::lock_guard lock(events->mutex); dispatcher = events->dispatcher; }
            auto guarded = [events, task = std::move(task)] {
                if (events->alive.load(std::memory_order_acquire)) task();
            };
            if (dispatcher) dispatcher(std::move(guarded)); else guarded();
        });
        playback_->onStateChanged = [this](State state) {
            forwardListener([state](Listener& listener) { listener.onStateChanged(state); });
        };
        playback_->onWakeRender = [this] { render_.setPlaying(true); };
        playback_->onFrameDecoded = [this](DecodedFrame packet) {
            if (!playback_->isCurrentGeneration(packet.generation)) return;
            lastPacket_ = std::move(packet);
            renderPacket();
        };
        playback_->onPositionChanged = [this](double seconds) {
            forwardListener([seconds](Listener& listener) { listener.onPositionChanged(seconds); });
        };
        playback_->onDurationChanged = [this](double seconds) {
            forwardListener([seconds](Listener& listener) { listener.onDurationChanged(seconds); });
        };
        playback_->onEndOfStream = [this] {
            forwardListener([](Listener& listener) { listener.onEndOfStream(); });
        };
        playback_->onOpenFailed = [this](const std::string& reason) {
            forwardListener([reason](Listener& listener) { listener.onOpenFailed(reason); });
        };
    }

    void resetPresentation() {
        const uint64_t generation = playback_->snapshot().generation;
        events_->generation.store(generation, std::memory_order_release);
        render_.post([this, generation] {
            if (!playback_->isCurrentGeneration(generation)) return;
            lastFrame_.reset();
            if (lastPacket_ && lastPacket_->generation != generation) lastPacket_.reset();
            if (frameRenderer_) { frameRenderer_->shutdown(); frameRenderer_.reset(); }
            if (lastPacket_) renderPacket();
        });
        render_.setPlaying(true);
    }

    void renderPacket() {
        if (!lastPacket_ || !presenter_ || !playback_->isCurrentGeneration(lastPacket_->generation)) return;
        const auto& packet = *lastPacket_;
        std::shared_ptr<VideoFrame> image;
        if (packet.sourceMode && !packet.blank) image = packet.source;
        else {
            if (!frameRenderer_) frameRenderer_ = std::make_unique<FrameRenderer>(packet.settings);
            else frameRenderer_->setSettings(packet.settings);
            std::string error;
            image = packet.blank ? frameRenderer_->background(packet.targetPosition, &error)
                : frameRenderer_->render(packet.source, packet.targetPosition, &error);
            if (!image) { LOG_ERROR("IPreviewer: render conversion failed: {}", error); return; }
        }
        if (image) presentDecodedFrame(image->native());
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
        notifyListener([path](Listener& listener) { listener.onFilterGraphChanged(path); });
        presentCurrentFrame();
        return true;
    }

    void applyGraphForFrame(const AVFrame*) {
        if (!presenter_) return;
        auto* wanted = filterGraph_.get();
        if (wanted == attachedGraph_) return;
        if (wanted) presenter_->setFilterGraph(wanted->graph(), wanted->input(), wanted->output());
        else presenter_->setFilterGraph(nullptr, nullptr, nullptr);
        attachedGraph_ = wanted;
    }

    void presentCurrentFrame() {
        if (!presenter_ || !lastFrame_ || !lastFrame_->data[0] || !lastPacket_
            || !playback_->isCurrentGeneration(lastPacket_->generation)) return;
        applyGraphForFrame(lastFrame_.get());
        if (!presenter_->presentFrame(lastFrame_.get())) {
            LOG_WARN("IPreviewer: presentFrame failed (format={}, {}x{})",
                     lastFrame_->format, lastFrame_->width, lastFrame_->height);
            return;
        }
        const auto generation = lastPacket_->generation;
        const double seconds = lastPacket_->targetPosition * lastPacket_->targetTimeBase.toDouble();
        events_->generation.store(playback_->snapshot().generation, std::memory_order_release);
        notifyListener([seconds](Listener& listener) { listener.onFramePresented(seconds); }, generation);
    }

    const filtergraph::VulkanGraphNodeDesc* findDocumentNode(
        uint64_t nodeId) const {
        for (const auto& node : graphDocument_.nodes()) {
            if (node.id == nodeId) return &node;
        }
        return nullptr;
    }

    void attachWindowOnRenderThread(void* nativeSurface, int width, int height) {
        if (!ensureGpu() || !gpuCtx_ || !playback_) return;
        detachWindowOnRenderThread();

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
        presenter_->setOnResize([this](int resizedWidth, int resizedHeight) {
            videoWidth_ = resizedWidth;
            videoHeight_ = resizedHeight;
        });
        attachedGraph_ = nullptr;
        applyGraphForFrame(lastFrame_.get());
        renderPacket();

        nativeSurface_ = nativeSurface;
        videoWidth_ = width;
        videoHeight_ = height;
        LOG_INFO("IPreviewer: attached native surface 0x{:x} {}x{}",
                 reinterpret_cast<uintptr_t>(nativeSurface), width, height);
    }

    void detachWindowOnRenderThread() {
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

    mutable renderer::RenderThread render_;
    std::shared_ptr<Events> events_ = std::make_shared<Events>();
    std::unique_ptr<VideoEngine> playback_;
    std::unique_ptr<FrameRenderer> frameRenderer_;
    std::optional<DecodedFrame> lastPacket_;
    std::unique_ptr<renderer::GpuContext> gpuCtx_;
    std::unique_ptr<renderer::VideoPresenter> presenter_;
    std::unique_ptr<filtergraph::VulkanFilterGraph> filterGraph_;
    filtergraph::VulkanGraphDocument graphDocument_;
    filtergraph::VulkanFilterGraph* attachedGraph_ = nullptr;
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
