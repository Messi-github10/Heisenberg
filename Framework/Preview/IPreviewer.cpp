#include "IPreviewer.hpp"

#include "VideoEngine.hpp"
#include <MultiMedia/Video/Renderer/CompositionRenderer.hpp>

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

#include <chrono>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>
#include <variant>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace heisenberg {
namespace {

using Clock = std::chrono::steady_clock;

double elapsedMs(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

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
        openSource(path, ColorSettings{}, {});
    }

    void openSource(const std::string& path, const ColorSettings& settings,
                    const std::string& mediaRef) override {
        std::string error;
        if (!validateColorSettings(settings, &error) || !supportedColorSettings(settings)) {
            if (error.empty()) error = "Unsupported preview source color settings";
            notifyListener([error](Listener& listener) { listener.onOpenFailed(error); });
            return;
        }
        // One render-worker transaction closes the old generation before any
        // newly decoded source can be paired with this project's color snapshot.
        // Async also lets the UI service Win32 messages if attach/resize is queued.
        render_.post([this, path, settings, mediaRef] {
            if (playback_) playback_->open(path);
            sourceColorSettings_ = settings;
            sourceMediaRef_ = mediaRef;
            sourceColorGeneration_ = playback_ ? playback_->snapshot().generation : 0;
            resetPresentation();
        });
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

    void openComposition(const CompositionPlan& plan) override {
        if (playback_) playback_->openComposition(plan);
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
            if (!compositionRenderer_) return;
            success = compositionRenderer_->setNodeParameter(nodeId, name, value);
            if (success) graphDocument_ = compositionRenderer_->filterGraphSnapshot();
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
            graphDocument_ = {};
            lastRendered_ = {};
            if (compositionRenderer_) { compositionRenderer_->shutdown(); compositionRenderer_.reset(); }
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

    filtergraph::VulkanGraphDocument filterGraphSnapshot() const override {
        filtergraph::VulkanGraphDocument snapshot;
        render_.postSync([this, &snapshot] { snapshot = graphDocument_; });
        return snapshot;
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
            vkRes.enabledFeatures = vkCtx.enabledDeviceFeatures();
            vkRes.enabledExtensions = vkCtx.enabledDeviceExtensions();
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
            reportedDiagnostics_.clear();
            lastRendered_ = {};
            if (lastPacket_ && lastPacket_->generation != generation) lastPacket_.reset();
            if (lastPacket_) renderPacket();
        });
        render_.setPlaying(true);
    }

    struct PlaybackTiming {
        uint64_t count = 0;
        double sumMs = 0.0;
        double maxMs = 0.0;
        int64_t worstTargetFrame = 0;

        void add(double ms, int64_t targetFrame) {
            if (ms < 0.0) return; // This stage was not reached.
            if (count == 0 || ms > maxMs) {
                maxMs = ms;
                worstTargetFrame = targetFrame;
            }
            ++count;
            sumMs += ms;
        }

        double averageMs() const { return count ? sumMs / count : 0.0; }
    };

    struct PlaybackStats {
        Clock::time_point lastSummary{};
        PlaybackTiming render, output, present, total;
        uint64_t fallbacks = 0;
        uint64_t failures = 0;
    };

    void recordPlaybackTiming(const DecodedFrame& packet, double renderMs,
                              double outputMs, double presentMs, double totalMs,
                              bool fallback, bool presented) {
        // Render-thread-only, retaining each clip's throttle when switching back.
        if (playbackStatsGeneration_ != packet.generation) {
            playbackStats_.clear();
            playbackStatsGeneration_ = packet.generation;
        }
        auto& stats = playbackStats_[packet.clipId];
        stats.render.add(renderMs, packet.targetPosition);
        stats.output.add(outputMs, packet.targetPosition);
        stats.present.add(presentMs, packet.targetPosition);
        stats.total.add(totalMs, packet.targetPosition);
        stats.fallbacks += fallback ? 1 : 0;
        stats.failures += presented ? 0 : 1;
        const auto now = Clock::now();
        if (stats.lastSummary != Clock::time_point{} &&
            now - stats.lastSummary < std::chrono::seconds(1)) return;

        const auto extent = lastRendered_.image().extent; // Metadata only; no GPU readback.
        LOG_INFO("[PlaybackDiag] render summary generation={} clipId='{}' mediaRef='{}' sourceMode={} "
                 "source={}x{} output={}x{} surface={}x{} target_frame={} count={} fallbacks={} failures={} "
                 "timings=API_wall_ms_not_GPU_timestamps "
                 "render_ms_wall(count/avg/max/worst_target_frame)={}/{:.3f}/{:.3f}/{} "
                 "output_ms_wall(count/avg/max/worst_target_frame)={}/{:.3f}/{:.3f}/{} "
                 "present_ms_wall(count/avg/max/worst_target_frame)={}/{:.3f}/{:.3f}/{} "
                 "total_ms_wall(count/avg/max/worst_target_frame)={}/{:.3f}/{:.3f}/{}",
                 packet.generation, packet.clipId, packet.mediaRef, packet.sourceMode,
                 packet.source ? packet.source->spec().width : 0,
                 packet.source ? packet.source->spec().height : 0,
                 extent.width, extent.height, videoWidth_, videoHeight_, packet.targetPosition,
                 stats.total.count, stats.fallbacks, stats.failures,
                 stats.render.count, stats.render.averageMs(), stats.render.maxMs, stats.render.worstTargetFrame,
                 stats.output.count, stats.output.averageMs(), stats.output.maxMs, stats.output.worstTargetFrame,
                 stats.present.count, stats.present.averageMs(), stats.present.maxMs, stats.present.worstTargetFrame,
                 stats.total.count, stats.total.averageMs(), stats.total.maxMs, stats.total.worstTargetFrame);
        Logger::ModuleLogger(__FILE__)->flush();
        stats = {};
        stats.lastSummary = Clock::now();
    }

    void renderPacket() {
        if (!lastPacket_ || !presenter_ || !compositionRenderer_
            || !playback_->isCurrentGeneration(lastPacket_->generation)) return;
        // API wall durations include existing internal waits, not GPU execution timestamps.
        // Total covers request preparation through output failure/present, excluding this summary.
        const auto totalStart = Clock::now();
        const auto& packet = *lastPacket_;
        const bool projectSourceColor = packet.sourceMode && sourceColorGeneration_ != 0
            && packet.generation >= sourceColorGeneration_;
        CompositionPlan sourcePlan;
        if (projectSourceColor) {
            if (packet.plan) sourcePlan = *packet.plan;
            sourcePlan.settings = packet.settings;
            sourcePlan.colorSettings = sourceColorSettings_;
        }
        renderer::CompositionRenderRequest request;
        request.plan = projectSourceColor ? &sourcePlan : packet.plan.get(); request.source = packet.source;
        if (projectSourceColor) {
            const auto overrideSpace = sourceColorSettings_.inputColorSpaces.find(sourceMediaRef_);
            if (overrideSpace != sourceColorSettings_.inputColorSpaces.end()) request.inputColorSpace = overrideSpace->second;
        }
        request.sourceId = projectSourceColor ? sourceMediaRef_ : packet.mediaRef;
        request.settings = packet.settings; request.targetFrame = packet.targetPosition;
        request.clipId = packet.clipId; request.clipLocalFrame = packet.clipLocalFrame;
        request.sourceMode = packet.sourceMode; request.blank = packet.blank;
        std::vector<std::string> diagnostics; request.diagnostics = &diagnostics;
        std::string error;
        lastRendered_ = {};
        auto renderStart = Clock::now();
        auto working = compositionRenderer_->render(request, &error);
        double renderMs = elapsedMs(renderStart);
        bool fallback = false;
        if (!working) {
            LOG_ERROR("IPreviewer: composition render failed: {}", error);
            notifyListener([error](Listener& listener) { listener.onFilterGraphFailed(error); });
            // Do not leave a stale successful frame visible after a failed request.
            request.blank = true; request.source.reset();
            fallback = true;
            renderStart = Clock::now();
            working = compositionRenderer_->render(request, nullptr);
            renderMs += elapsedMs(renderStart);
        }
        renderer::ColorOutputRequest output;
        if (request.plan) {
            output.display = request.plan->colorSettings.previewOutput.display;
            output.view = request.plan->colorSettings.previewOutput.view;
            if (request.plan->colorSettings.inputPolicyVersion == 2 &&
                output.display == "sRGB - Display" && output.view == "Un-tone-mapped")
                output.intent = renderer::ColorOutputIntent::SdrVideoPreview;
        } else output.intent = renderer::ColorOutputIntent::SdrVideoPreview;
        double outputMs = -1.0;
        if (working) {
            const auto outputStart = Clock::now();
            lastRendered_ = compositionRenderer_->output(working, output, &error);
            outputMs = elapsedMs(outputStart);
        }
        if (!lastRendered_) {
            LOG_ERROR("IPreviewer: output transform failed: {}", error);
            notifyListener([error](Listener& listener) { listener.onFilterGraphFailed(error); });
            recordPlaybackTiming(packet, renderMs, outputMs, -1.0, elapsedMs(totalStart), fallback, false);
            return;
        }
        std::string diagnosticSource = projectSourceColor ? sourceMediaRef_ : packet.mediaRef;
        if (diagnosticSource.empty() && request.plan) {
            for (const auto& clip : request.plan->clips) {
                if (packet.sourceMode || clip.id == packet.clipId) {
                    diagnosticSource = clip.sourcePath;
                    break;
                }
            }
        }
        for (const auto& diagnostic : diagnostics) {
            if (!reportedDiagnostics_.emplace(diagnosticSource, diagnostic).second) continue;
            LOG_WARN("IPreviewer: {}", diagnostic);
            notifyListener([diagnostic](Listener& listener) { listener.onFilterGraphFailed(diagnostic); }, packet.generation);
        }
        const auto presentStart = Clock::now();
        const bool presented = presenter_->presentFrame(lastRendered_);
        const double presentMs = elapsedMs(presentStart);
        recordPlaybackTiming(packet, renderMs, outputMs, presentMs, elapsedMs(totalStart), fallback, presented);
        if (!presented) return;
        const auto generation = packet.generation;
        const double seconds = packet.targetPosition * packet.targetTimeBase.toDouble();
        events_->generation.store(playback_->snapshot().generation, std::memory_order_release);
        notifyListener([seconds](Listener& listener) { listener.onFramePresented(seconds); }, generation);
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

        filtergraph::VulkanGraphDocument graphDocument;
        if (!graphDocument.loadFromJsonFile(path, error)) return false;
        if (!compositionRenderer_ || !compositionRenderer_->setFilterGraphSnapshot(graphDocument, error)) return false;
        graphDocument_ = std::move(graphDocument);
        filterGraphPath_ = path;
        notifyListener([path](Listener& listener) { listener.onFilterGraphChanged(path); });
        presentCurrentFrame();
        return true;
    }

    void presentCurrentFrame() { renderPacket(); }

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

        if (!compositionRenderer_) {
            compositionRenderer_ = std::make_unique<renderer::CompositionRenderer>();
            std::string error;
            if (!compositionRenderer_->initialize(gpuCtx_->plGpu(), gpuCtx_->plVulkan(),
                    renderer::D3D11Context::instance().device(),
                    renderer::D3D11Context::instance().context(), &error)) {
                LOG_ERROR("IPreviewer: CompositionRenderer initialization failed: {}", error);
                compositionRenderer_.reset(); presenter_.reset(); return;
            }
            if (!graphDocument_.nodes().empty()) compositionRenderer_->setFilterGraphSnapshot(graphDocument_, &error);
        }
        presenter_->setOnResize([this](int resizedWidth, int resizedHeight) {
            videoWidth_ = resizedWidth;
            videoHeight_ = resizedHeight;
        });
        renderPacket();

        nativeSurface_ = nativeSurface;
        videoWidth_ = width;
        videoHeight_ = height;
        LOG_INFO("IPreviewer: attached native surface 0x{:x} {}x{}",
                 reinterpret_cast<uintptr_t>(nativeSurface), width, height);
    }

    void detachWindowOnRenderThread() {
        if (presenter_) {
            presenter_->shutdown();
            presenter_.reset();
        }
        nativeSurface_ = nullptr;
        lastRendered_ = {};
        videoWidth_ = 0;
        videoHeight_ = 0;
    }

    mutable renderer::RenderThread render_;
    std::shared_ptr<Events> events_ = std::make_shared<Events>();
    std::unique_ptr<VideoEngine> playback_;
    std::unique_ptr<renderer::CompositionRenderer> compositionRenderer_;
    std::optional<DecodedFrame> lastPacket_;
    std::unique_ptr<renderer::GpuContext> gpuCtx_;
    std::unique_ptr<renderer::VideoPresenter> presenter_;
    filtergraph::VulkanGraphDocument graphDocument_;
    renderer::RenderedFrame lastRendered_;
    std::set<std::pair<std::string, std::string>> reportedDiagnostics_;
    std::map<std::string, PlaybackStats> playbackStats_;
    uint64_t playbackStatsGeneration_ = 0;
    ColorSettings sourceColorSettings_;
    std::string sourceMediaRef_;
    uint64_t sourceColorGeneration_ = 0;
    std::string filterGraphPath_;
    void* nativeSurface_ = nullptr;
    int videoWidth_ = 0;
    int videoHeight_ = 0;
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
