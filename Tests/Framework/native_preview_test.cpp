#include <Preview/IPreviewer.hpp>
#include <Models/Timeline.hpp>
#include <Models/MediaManifest.hpp>
#include <MultiMedia/Video/Renderer/RenderThread.hpp>
#include <Platform/Vulkan/VulkanContext.hpp>
#include <Utiles/Logger.hpp>
#include <spdlog/sinks/base_sink.h>

#include <gtest/gtest.h>
#include <windows.h>

#include <chrono>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace {

struct PreviewWindow {
    HWND handle = nullptr;
    PreviewWindow() {
        handle = CreateWindowExW(0, L"STATIC", L"Heisenberg renderer integration",
            WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 40, 400, 300,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    }
    ~PreviewWindow() { if (handle) DestroyWindow(handle); }
};

class DiagnosticLogSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    std::map<std::string, int> counts() {
        std::lock_guard lock(mutex_);
        return counts_;
    }
private:
    void sink_it_(const spdlog::details::log_msg& message) override {
        if (message.level != spdlog::level::warn) return;
        const std::string text(message.payload.data(), message.payload.size());
        if (text.starts_with("IPreviewer: ")) ++counts_[text.substr(12)];
    }
    void flush_() override {}
    std::map<std::string, int> counts_;
};

class CapturePreviewLogs {
public:
    CapturePreviewLogs() {
        logger_ = heisenberg::Logger::ModuleLogger(
            "Framework/Preview/IPreviewer.cpp");
        sinks_ = logger_->sinks();
        logger_->sinks().push_back(sink);
    }
    ~CapturePreviewLogs() { logger_->sinks() = std::move(sinks_); }
    std::shared_ptr<DiagnosticLogSink> sink = std::make_shared<DiagnosticLogSink>();
private:
    spdlog::logger* logger_ = nullptr;
    std::vector<spdlog::sink_ptr> sinks_;
};

class RendererColorPathLogSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    std::vector<std::string> paths() {
        std::lock_guard lock(mutex_);
        return paths_;
    }
private:
    void sink_it_(const spdlog::details::log_msg& message) override {
        if (message.level != spdlog::level::info) return;
        const std::string text(message.payload.data(),message.payload.size());
        if (text.starts_with("CompositionRenderer color path: ")) paths_.push_back(text);
    }
    void flush_() override {}
    std::vector<std::string> paths_;
};

class CaptureRendererColorPaths {
public:
    CaptureRendererColorPaths() {
        logger_ = heisenberg::Logger::ModuleLogger(
            "Framework/MultiMedia/Video/Renderer/CompositionRenderer.cpp");
        sinks_ = logger_->sinks();
        level_ = logger_->level();
        logger_->sinks().push_back(sink);
        logger_->set_level(spdlog::level::info);
    }
    ~CaptureRendererColorPaths() {
        logger_->sinks() = std::move(sinks_);
        logger_->set_level(level_);
    }
    std::shared_ptr<RendererColorPathLogSink> sink = std::make_shared<RendererColorPathLogSink>();
private:
    spdlog::logger* logger_ = nullptr;
    std::vector<spdlog::sink_ptr> sinks_;
    spdlog::level::level_enum level_ = spdlog::level::info;
};

struct PresentationListener final : heisenberg::IPreviewer::Listener {
    int frames = 0;
    std::map<std::string, int> diagnostics;
    double seconds = -1.0;
    std::thread::id uiThread = std::this_thread::get_id();
    bool allNotificationsOnUi = true;
    void onFilterGraphFailed(const std::string& message) override {
        ++diagnostics[message];
    }
    void onFramePresented(double value) override {
        ++frames;
        seconds = value;
        allNotificationsOnUi = allNotificationsOnUi && std::this_thread::get_id() == uiThread;
    }
};

class UiTasks {
public:
    void push(heisenberg::IPreviewer::Task task) {
        std::lock_guard lock(mutex_);
        tasks_.push_back(std::move(task));
    }
    void process() {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        std::deque<heisenberg::IPreviewer::Task> tasks;
        { std::lock_guard lock(mutex_); tasks.swap(tasks_); }
        for (auto& task : tasks) task();
    }
    bool wait(const std::function<bool()>& predicate) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        do {
            process();
            if (predicate()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
    }
private:
    std::mutex mutex_;
    std::deque<heisenberg::IPreviewer::Task> tasks_;
};

} // namespace

TEST(NativePreviewTest, ColorDiagnosticsAreReportedOnceUntilSourceOrColorChanges) {
    heisenberg::IPreviewer::initLoader();
    PreviewWindow window;
    ASSERT_NE(window.handle, nullptr);
    UiTasks tasks;
    PresentationListener listener;
    CapturePreviewLogs logs;
    auto preview = heisenberg::IPreviewer::create();
    preview->setTaskDispatcher([&](auto task) { tasks.push(std::move(task)); });
    preview->setListener(&listener);
    preview->setHardwareDecode(false);
    preview->attachWindow(window.handle, 320, 240);
    preview->open(HEISENBERG_TEST_MEDIA);
    ASSERT_TRUE(tasks.wait([&] { return listener.frames > 0 && preview->state() == heisenberg::IPreviewer::State::Paused; }));
    const auto first = logs.sink->counts();
    ASSERT_FALSE(first.empty());
    EXPECT_TRUE(std::any_of(first.begin(), first.end(), [](const auto& entry) {
        return entry.first.find("Unsupported or absent source primaries/transfer") != std::string::npos;
    }));
    for (const auto& [message, count] : first) {
        EXPECT_EQ(count, 1) << message;
        EXPECT_EQ(listener.diagnostics[message], 1) << message;
    }
    auto waitForFrame = [&](const auto& action) {
        const int before = listener.frames;
        action();
        return tasks.wait([&] { return listener.frames > before; });
    };
    ASSERT_TRUE(waitForFrame([&] { preview->resize(360, 260); }));
    ASSERT_TRUE(waitForFrame([&] { preview->seek(0.5); }));
    preview->play();
    ASSERT_TRUE(tasks.wait([&] { return preview->state() == heisenberg::IPreviewer::State::Ended; }));
    EXPECT_EQ(logs.sink->counts(), first);
    for (const auto& [message, count] : first) EXPECT_EQ(listener.diagnostics[message], count);

    heisenberg::ColorSettings overridden;
    overridden.inputColorSpaces["asset"] = "Rec.1886 Rec.709 - Display";
    ASSERT_TRUE(waitForFrame([&] { preview->openSource(HEISENBERG_TEST_MEDIA, overridden, "asset"); }));
    ASSERT_TRUE(waitForFrame([&] { preview->resize(320, 240); }));
    EXPECT_EQ(logs.sink->counts(), first);

    ASSERT_TRUE(waitForFrame([&] { preview->open(HEISENBERG_TEST_MEDIA); }));
    for (const auto& [message, count] : logs.sink->counts()) {
        EXPECT_EQ(count, 2) << message;
        EXPECT_EQ(listener.diagnostics[message], 2) << message;
    }
    preview->shutdown();
    tasks.process();
    EXPECT_EQ(heisenberg::renderer::VulkanContext::instance().validationErrorCount(), 0u);
}

TEST(NativePreviewTest, OrdinaryProjectSourceWiresCodePreservingSdrIntentToActualRendererOutput) {
    heisenberg::IPreviewer::initLoader();
    PreviewWindow window;
    ASSERT_NE(window.handle,nullptr);
    UiTasks tasks;
    PresentationListener listener;
    // Attach only to the renderer module: existing preview warning counts and
    // diagnostics remain independent of this INFO-level wiring regression.
    CaptureRendererColorPaths logs;
    auto preview = heisenberg::IPreviewer::create();
    preview->setTaskDispatcher([&](auto task) { tasks.push(std::move(task)); });
    preview->setListener(&listener);
    preview->setHardwareDecode(false);
    preview->attachWindow(window.handle,320,240);
    constexpr const char* mediaRef = "ordinary-sdr-intent-wiring";
    preview->openSource(HEISENBERG_TEST_MEDIA,heisenberg::ColorSettings{},mediaRef);
    const bool presented = tasks.wait([&] {
        return listener.frames > 0 && preview->state() == heisenberg::IPreviewer::State::Paused;
    });
    // Join/drain GPU work before detaching or restoring the logger sinks, even
    // on a failed presentation. This avoids changing a worker's live sink list.
    preview->shutdown();
    tasks.process();
    preview.reset();
    const auto paths = logs.sink->paths();
    ASSERT_TRUE(presented);
    ASSERT_FALSE(paths.empty());
    const auto path = std::find_if(paths.begin(),paths.end(),[](const auto& message) {
        return message.find("source='ordinary-sdr-intent-wiring'") != std::string::npos;
    });
    ASSERT_NE(path,paths.end());
    // The fixture's unknown YUV metadata uses the ordinary policy2 fallback;
    // this must take the same route as tagged finished Rec709 video.
    for (const char* field : {"policy=2", "selectionOrigin=fallback",
             "interpretedInput='Rec.1886 Rec.709 - Display'",
             "outputDisplay='Rec.1886 Rec.709 - Display'", "outputView='Un-tone-mapped'",
             "outputColorSpace=''", "presentationIntent=SDR video code-preserving preview"})
        EXPECT_NE(path->find(field),std::string::npos) << field << "\n" << *path;
    EXPECT_TRUE(listener.allNotificationsOnUi);
    EXPECT_EQ(heisenberg::renderer::VulkanContext::instance().validationErrorCount(),0u);
}

TEST(NativePreviewTest, RealWindowPresentsSourceAndTimelineOnRenderWorker) {
    heisenberg::IPreviewer::initLoader();
    PreviewWindow window;
    ASSERT_NE(window.handle, nullptr);
    UiTasks tasks;
    PresentationListener listener;
    auto preview = heisenberg::IPreviewer::create();
    preview->setTaskDispatcher([&](auto task) { tasks.push(std::move(task)); });
    preview->setListener(&listener);
    preview->setHardwareDecode(false);
    preview->attachWindow(window.handle, 320, 240);
    preview->open(HEISENBERG_TEST_MEDIA);
    ASSERT_TRUE(tasks.wait([&] { return listener.frames > 0 && preview->state() == heisenberg::IPreviewer::State::Paused; }));
    int frames = listener.frames;
    preview->seek(0.5);
    ASSERT_TRUE(tasks.wait([&] { return listener.frames > frames && listener.seconds >= 0.49; }));
    frames = listener.frames;
    preview->resize(360, 260);
    ASSERT_TRUE(tasks.wait([&] { return listener.frames > frames; }));
    preview->play();
    ASSERT_TRUE(tasks.wait([&] { return preview->state() == heisenberg::IPreviewer::State::Ended; }));
    preview->play();
    ASSERT_TRUE(tasks.wait([&] { return preview->state() == heisenberg::IPreviewer::State::Playing; }));
    preview->pause();
    frames = listener.frames;
    preview->setHardwareDecode(true);
    preview->open(HEISENBERG_TEST_MEDIA);
    ASSERT_TRUE(tasks.wait([&] { return listener.frames > frames && preview->state() == heisenberg::IPreviewer::State::Paused; }));

    heisenberg::Timeline timeline;
    timeline.id = "native-timeline";
    timeline.fps = 24;
    timeline.width = 320;
    timeline.height = 240;
    heisenberg::Track track;
    track.id = "native-track";
    heisenberg::Clip clip;
    clip.id = "native-clip";
    clip.mediaRef = "native-media";
    clip.durationFrames = 24;
    track.clips.push_back(clip);
    timeline.tracks.push_back(track);
    heisenberg::MediaManifest manifest;
    heisenberg::MediaManifestEntry asset;
    asset.id = clip.mediaRef;
    asset.name = "native source";
    asset.duration = 1.0;
    asset.source.path = HEISENBERG_TEST_MEDIA;
    asset.source.kind = heisenberg::MediaSource::Kind::External;
    ASSERT_TRUE(manifest.add(asset));
    frames = listener.frames;
    preview->openTimeline(timeline, manifest, {});
    ASSERT_TRUE(tasks.wait([&] { return listener.frames > frames && preview->state() == heisenberg::IPreviewer::State::Paused; }));
    preview->beginScrub();
    preview->scrubToFrame(12);
    ASSERT_TRUE(tasks.wait([&] { return listener.seconds >= 0.49; }));
    preview->endScrub(3);
    ASSERT_TRUE(tasks.wait([&] { return preview->state() == heisenberg::IPreviewer::State::Paused && listener.seconds < 0.2; }));
    EXPECT_TRUE(listener.allNotificationsOnUi);
    frames = listener.frames;
    preview->openFilterGraph(HEISENBERG_TEST_FILTER);
    ASSERT_TRUE(tasks.wait([&] { return listener.frames > frames; }));
    EXPECT_EQ(preview->filterGraphPath(), HEISENBERG_TEST_FILTER);
    frames = listener.frames;
    ASSERT_TRUE(preview->setNodeParameter(3, "exposure", 0.5f));
    ASSERT_TRUE(tasks.wait([&] { return listener.frames > frames; }));
    preview->shutdown();
    tasks.process();
    EXPECT_EQ(heisenberg::renderer::VulkanContext::instance().validationErrorCount(), 0u);
}

TEST(NativePreviewTest, ProjectSourceColorSnapshotPreservesSourceTiming) {
    heisenberg::IPreviewer::initLoader();
    PreviewWindow window;
    ASSERT_NE(window.handle, nullptr);
    UiTasks tasks;
    PresentationListener listener;
    auto preview = heisenberg::IPreviewer::create();
    preview->setTaskDispatcher([&](auto task) { tasks.push(std::move(task)); });
    preview->setListener(&listener);
    preview->setHardwareDecode(false);
    preview->attachWindow(window.handle, 320, 240);
    preview->open(HEISENBERG_TEST_MEDIA);
    ASSERT_TRUE(tasks.wait([&] { return listener.frames > 0 && preview->state() == heisenberg::IPreviewer::State::Paused; }));
    const double fps = preview->fps(), duration = preview->duration();
    const int64_t count = preview->frameCount();
    int frames = listener.frames;
    heisenberg::ColorSettings projectColor;
    projectColor.inputColorSpaces["asset"] = "ACEScg";
    projectColor.previewOutput = {"Rec.1886 Rec.709 - Display", "ACES 2.0 - SDR 100 nits (Rec.709)"};
    preview->openSource(HEISENBERG_TEST_MEDIA, projectColor, "asset");
    ASSERT_TRUE(tasks.wait([&] { return listener.frames > frames && preview->state() == heisenberg::IPreviewer::State::Paused; }));
    EXPECT_DOUBLE_EQ(preview->fps(), fps);
    EXPECT_DOUBLE_EQ(preview->duration(), duration);
    EXPECT_EQ(preview->frameCount(), count);
    frames = listener.frames;
    preview->seek(0.5);
    ASSERT_TRUE(tasks.wait([&] { return listener.frames > frames && listener.seconds >= 0.49; }));
    EXPECT_TRUE(listener.allNotificationsOnUi);
    preview->shutdown();
    tasks.process();
    EXPECT_EQ(heisenberg::renderer::VulkanContext::instance().validationErrorCount(), 0u);
}

TEST(NativePreviewTest, AudioSourceAndAudioTailReplaceVideoWithBlackCanvas) {
    heisenberg::IPreviewer::initLoader();
    PreviewWindow window;
    ASSERT_NE(window.handle, nullptr);
    UiTasks tasks;
    PresentationListener listener;
    auto preview = heisenberg::IPreviewer::create();
    preview->setTaskDispatcher([&](auto task) { tasks.push(std::move(task)); });
    preview->setListener(&listener);
    preview->setHardwareDecode(false);
    preview->attachWindow(window.handle, 320, 240);
    preview->open(HEISENBERG_TEST_MEDIA);
    ASSERT_TRUE(tasks.wait([&] { return listener.frames > 0 && preview->state() == heisenberg::IPreviewer::State::Paused; }));
    int frames = listener.frames;
    preview->open(HEISENBERG_TEST_COVER_MP3);
    ASSERT_TRUE(tasks.wait([&] { return listener.frames > frames && preview->state() == heisenberg::IPreviewer::State::Paused; }));
    EXPECT_GT(preview->duration(), 1.0);
    preview->seek(0.5);
    ASSERT_TRUE(tasks.wait([&] { return listener.seconds >= 0.49; }));
    preview->play();
    ASSERT_TRUE(tasks.wait([&] { return preview->state() == heisenberg::IPreviewer::State::Ended; }));

    heisenberg::Timeline timeline;
    timeline.id = "audio-tail-timeline";
    timeline.fps = 24;
    timeline.width = 320;
    timeline.height = 240;
    heisenberg::Track videoTrack;
    videoTrack.id = "video-track";
    heisenberg::Clip videoClip;
    videoClip.id = "video-clip";
    videoClip.mediaRef = "video-source";
    videoClip.durationFrames = 24;
    videoTrack.clips.push_back(videoClip);
    heisenberg::Track audioTrack;
    audioTrack.id = "audio-track";
    audioTrack.type = heisenberg::ClipType::Audio;
    heisenberg::Clip audioClip;
    audioClip.id = "audio-clip";
    audioClip.mediaRef = "audio-source";
    audioClip.mediaType = heisenberg::ClipType::Audio;
    audioClip.sourceClipType = heisenberg::ClipType::Audio;
    audioClip.durationFrames = 72;
    audioTrack.clips.push_back(audioClip);
    timeline.tracks = {videoTrack, audioTrack};
    heisenberg::MediaManifest manifest;
    heisenberg::MediaManifestEntry video;
    video.id = videoClip.mediaRef;
    video.name = "video";
    video.duration = 1.0;
    video.source.path = HEISENBERG_TEST_MEDIA;
    ASSERT_TRUE(manifest.add(video));
    heisenberg::MediaManifestEntry audio;
    audio.id = audioClip.mediaRef;
    audio.name = "audio";
    audio.type = heisenberg::ClipType::Audio;
    audio.duration = 3.0;
    audio.source.path = HEISENBERG_TEST_LONG_AUDIO;
    ASSERT_TRUE(manifest.add(audio));
    frames = listener.frames;
    preview->openTimeline(timeline, manifest, {});
    ASSERT_TRUE(tasks.wait([&] { return listener.frames > frames && preview->state() == heisenberg::IPreviewer::State::Paused; }));
    EXPECT_NEAR(preview->duration(), 3.0, 1e-6);
    preview->seek(2.5);
    ASSERT_TRUE(tasks.wait([&] { return listener.seconds >= 2.49; }));
    preview->play();
    ASSERT_TRUE(tasks.wait([&] { return preview->state() == heisenberg::IPreviewer::State::Ended; }));
    EXPECT_NEAR(preview->currentTime(), 3.0, 1.0 / 24.0);
    EXPECT_TRUE(listener.allNotificationsOnUi);
    preview->shutdown();
    tasks.process();
    EXPECT_EQ(heisenberg::renderer::VulkanContext::instance().validationErrorCount(), 0u);
}
