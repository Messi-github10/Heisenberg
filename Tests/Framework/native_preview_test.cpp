#include <Preview/IPreviewer.hpp>
#include <Models/Timeline.hpp>
#include <Models/MediaManifest.hpp>
#include <MultiMedia/Video/Renderer/RenderThread.hpp>

#include <gtest/gtest.h>
#include <windows.h>

#include <chrono>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

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

struct PresentationListener final : heisenberg::IPreviewer::Listener {
    int frames = 0;
    double seconds = -1.0;
    std::thread::id uiThread = std::this_thread::get_id();
    bool allNotificationsOnUi = true;
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
}
