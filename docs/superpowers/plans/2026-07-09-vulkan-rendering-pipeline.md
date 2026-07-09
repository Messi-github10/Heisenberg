# Vulkan Rendering Pipeline — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace CPU rendering (sws_scale → QImage → FrameImageProvider → QML Image) with GPU pipeline (FFmpeg decode → libplacebo Vulkan → child HWND embedded in QML).

**Architecture:** Move TextureTransfer to render/ namespace. Create VideoPreviewer (pure C++) that wires GpuContext → SwapChain → RenderEngine → TextureTransfer. Create VideoOutputItem (QQuickItem) that manages a native child HWND. Refactor PlaybackController to emit AVFrame directly. Wire PlayerController to connect the new components.

**Tech Stack:** C++20, Vulkan 1.3, libplacebo v7, Qt 6.8, FFmpeg 6.2

## Global Constraints

- C++20, CMake 3.21+, MSVC 2022
- Previewer layer: pure C++, no Qt dependency
- UserInterface layer: Qt 6.8 QML + QQuickItem
- Vulkan 1.3 via Volk + Vulkan-Hpp dynamic dispatch
- libplacebo v7 for GPU rendering (YUV→RGB)
- FFmpeg 6.2 for demuxing + software decoding only
- Windows only (VK_USE_PLATFORM_WIN32_KHR)
- Correctness first; performance optimization NOT in scope

---

### Task 1: Move TextureTransfer to render/ namespace

**Files:**
- Create: `Previewer/Render/TextureTransfer.hpp` (move from Decoder/)
- Create: `Previewer/Render/TextureTransfer.cpp` (move from Decoder/)
- Modify: `Previewer/CMakeLists.txt:14-15` (update paths)

**Interfaces:**
- Consumes: nothing (standalone)
- Produces: `heisenberg::render::TextureTransfer` class with `uploadAVFrame(const AVFrame*) → const pl_frame*`

- [ ] **Step 1: Read the original TextureTransfer files**

Read `Previewer/Decoder/TextureTransfer.hpp` and `Previewer/Decoder/TextureTransfer.cpp` to confirm current content.

- [ ] **Step 2: Write new TextureTransfer.hpp with render namespace**

```cpp
//
// Created by NiceFold on 2026/6/30.
//

#pragma once

#include <memory>

struct AVFrame;
struct pl_frame;

extern "C" {
#include <libplacebo/gpu.h>
}

namespace heisenberg {
namespace render {

class TextureTransfer {
public:
    explicit TextureTransfer(pl_gpu gpu);
    ~TextureTransfer();

    TextureTransfer(const TextureTransfer&) = delete;
    TextureTransfer& operator=(const TextureTransfer&) = delete;

    const pl_frame* uploadAVFrame(const AVFrame* avframe);

private:
    void releaseTextures();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace render
} // namespace heisenberg
```

- [ ] **Step 3: Write new TextureTransfer.cpp with render namespace**

Copy the full content of `Previewer/Decoder/TextureTransfer.cpp`, changing only:
- Line 5: `#include "TextureTransfer.hpp"` stays the same
- Line 21: `namespace heisenberg { namespace decoder {` → `namespace heisenberg { namespace render {`
- Line 390: `} // namespace decoder` → `} // namespace render`

```cpp
//
// Created by NiceFold on 2026/6/30.
//

#include "TextureTransfer.hpp"

#include <Utiles/Logger.hpp>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
}

#include <libplacebo/renderer.h>
#include <libplacebo/gpu.h>

#include <cstring>
#include <stdexcept>

namespace heisenberg {
namespace render {

// ... (entire existing implementation unchanged, only namespace changed) ...

} // namespace render
} // namespace heisenberg
```

The full implementation is identical to the original — only the namespace and closing comment change.

- [ ] **Step 4: Delete old TextureTransfer files**

Remove `Previewer/Decoder/TextureTransfer.hpp` and `Previewer/Decoder/TextureTransfer.cpp`.

- [ ] **Step 5: Update Previewer/CMakeLists.txt**

Change lines 14-15 from:
```cmake
        Decoder/TextureTransfer.cpp
        Decoder/TextureTransfer.hpp
```
To:
```cmake
        Render/TextureTransfer.cpp
        Render/TextureTransfer.hpp
```

- [ ] **Step 6: Build and verify**

```bash
cd build && cmake --build . --target HeisenbergPreviewer
```
Expected: build succeeds, TextureTransfer now in `heisenberg::render` namespace.

- [ ] **Step 7: Commit**

```bash
git add Previewer/Render/TextureTransfer.hpp Previewer/Render/TextureTransfer.cpp Previewer/CMakeLists.txt
git rm Previewer/Decoder/TextureTransfer.hpp Previewer/Decoder/TextureTransfer.cpp
git commit -m "refactor: move TextureTransfer from decoder to render namespace"
```

---

### Task 2: Create VideoPreviewer (pure C++ render pipeline)

**Files:**
- Create: `Previewer/Render/VideoPreviewer.hpp`
- Create: `Previewer/Render/VideoPreviewer.cpp`
- Modify: `Previewer/CMakeLists.txt` (add new sources)

**Interfaces:**
- Consumes: `GpuContext`, `SwapChain`, `RenderEngine`, `TextureTransfer`, `VulkanContext` (all existing in render/)
- Produces: `heisenberg::render::VideoPreviewer` class

- [ ] **Step 1: Write VideoPreviewer.hpp**

```cpp
//
// Created by NiceFold on 2026/7/9.
//

#pragma once

#include <memory>
#include <functional>

#ifdef VK_USE_PLATFORM_WIN32_KHR
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

struct AVFrame;

namespace heisenberg {
namespace render {

/// VideoPreviewer — 纯 C++ Vulkan 渲染管线
///
/// 连线：TextureTransfer → RenderEngine → SwapChain → 子 HWND
/// 无 Qt 依赖，通过 std::function 回调通知 UI 层。
class VideoPreviewer {
public:
    using ResizeCallback  = std::function<void(int width, int height)>;
    using PresentCallback = std::function<void()>;

    VideoPreviewer();
    ~VideoPreviewer();

    VideoPreviewer(const VideoPreviewer&)            = delete;
    VideoPreviewer& operator=(const VideoPreviewer&) = delete;
    VideoPreviewer(VideoPreviewer&&)                 = delete;
    VideoPreviewer& operator=(VideoPreviewer&&)      = delete;

    /// 初始化渲染管线：创建 GpuContext + VkSurface + SwapChain + RenderEngine + TextureTransfer
    /// @param hwnd   子 HWND（来自 VideoOutputItem）
    /// @param width  初始宽度
    /// @param height 初始高度
    /// @return true 表示初始化成功
    bool initialize(HWND hwnd, int width, int height);

    /// 渲染一帧
    /// @param avframe FFmpeg 解码的 AVFrame（CPU 数据）
    /// @return true 表示渲染成功
    bool presentFrame(const AVFrame* avframe);

    /// 窗口大小变化时重建 swapchain
    void resize(int width, int height);

    /// 设置回调
    void setOnResize(ResizeCallback cb);
    void setOnPresent(PresentCallback cb);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace render
} // namespace heisenberg
```

- [ ] **Step 2: Write VideoPreviewer.cpp**

```cpp
//
// Created by NiceFold on 2026/7/9.
//

#include "VideoPreviewer.hpp"

#include "GpuContext.hpp"
#include "SwapChain.hpp"
#include "RenderEngine.hpp"
#include "TextureTransfer.hpp"
#include "VulkanContext.hpp"

#include <Utiles/Logger.hpp>

namespace heisenberg {
namespace render {

struct VideoPreviewer::Impl {
    std::unique_ptr<GpuContext>      gpuCtx;           // 需最先析构，故声明在最前
    std::unique_ptr<SwapChain>       swapChain;
    std::unique_ptr<RenderEngine>    renderEngine;
    std::unique_ptr<TextureTransfer> textureTransfer;

    ResizeCallback  onResize;
    PresentCallback onPresent;

    bool initialized = false;
};

VideoPreviewer::VideoPreviewer()
    : impl_(std::make_unique<Impl>()) {}

VideoPreviewer::~VideoPreviewer() = default;

bool VideoPreviewer::initialize(HWND hwnd, int width, int height) {
    if (!hwnd) {
        LOG_ERROR("VideoPreviewer: invalid HWND");
        return false;
    }

    // 1. GpuContext（封装 pl_vulkan）
    try {
        impl_->gpuCtx = std::make_unique<GpuContext>();
    } catch (const std::exception& e) {
        LOG_ERROR("VideoPreviewer: GpuContext creation failed — {}", e.what());
        return false;
    }

    // 2. 从子 HWND 创建 VkSurfaceKHR
    auto& vkCtx = VulkanContext::instance();
    VkSurfaceKHR surface = vkCtx.createSurface(hwnd);
    if (surface == VK_NULL_HANDLE) {
        LOG_ERROR("VideoPreviewer: createSurface() failed");
        impl_->gpuCtx.reset();
        return false;
    }

    // 3. SwapChain（接管 surface 所有权）
    try {
        impl_->swapChain = std::make_unique<SwapChain>(
            impl_->gpuCtx->vulkan(), surface, width, height);
    } catch (const std::exception& e) {
        LOG_ERROR("VideoPreviewer: SwapChain creation failed — {}", e.what());
        impl_->gpuCtx.reset();
        return false;
    }

    // 4. RenderEngine
    try {
        impl_->renderEngine = std::make_unique<RenderEngine>(impl_->gpuCtx->gpu());
    } catch (const std::exception& e) {
        LOG_ERROR("VideoPreviewer: RenderEngine creation failed — {}", e.what());
        impl_->swapChain.reset();
        impl_->gpuCtx.reset();
        return false;
    }

    // 5. TextureTransfer
    try {
        impl_->textureTransfer = std::make_unique<TextureTransfer>(impl_->gpuCtx->gpu());
    } catch (const std::exception& e) {
        LOG_ERROR("VideoPreviewer: TextureTransfer creation failed — {}", e.what());
        impl_->renderEngine.reset();
        impl_->swapChain.reset();
        impl_->gpuCtx.reset();
        return false;
    }

    impl_->initialized = true;
    LOG_INFO("VideoPreviewer: initialized — {}x{}", width, height);
    return true;
}

bool VideoPreviewer::presentFrame(const AVFrame* avframe) {
    if (!impl_->initialized) {
        return false;
    }

    // 1. 开始帧
    if (!impl_->swapChain->startFrame()) {
        LOG_WARN("VideoPreviewer: startFrame() failed — swapchain may need resize");
        return false;
    }

    // 2. 上传 AVFrame → GPU 纹理
    const pl_frame* srcFrame = impl_->textureTransfer->uploadAVFrame(avframe);
    if (!srcFrame) {
        // uploadAVFrame 已记录错误日志
        // 仍需提交帧以释放 swapchain 资源
        impl_->swapChain->submitFrame();
        return false;
    }

    // 3. 渲染（GPU YUV→RGB）
    const pl_frame* target = impl_->swapChain->getTargetFrame();
    bool ok = impl_->renderEngine->render(srcFrame, target);

    // 4. 提交帧
    impl_->swapChain->submitFrame();

    if (ok && impl_->onPresent) {
        impl_->onPresent();
    }

    return ok;
}

void VideoPreviewer::resize(int width, int height) {
    if (!impl_->initialized || !impl_->swapChain) {
        return;
    }

    int w = width, h = height;
    if (impl_->swapChain->resize(&w, &h)) {
        LOG_DEBUG("VideoPreviewer: resized to {}x{}", w, h);
        if (impl_->onResize) {
            impl_->onResize(w, h);
        }
    }
}

void VideoPreviewer::setOnResize(ResizeCallback cb) {
    impl_->onResize = std::move(cb);
}

void VideoPreviewer::setOnPresent(PresentCallback cb) {
    impl_->onPresent = std::move(cb);
}

} // namespace render
} // namespace heisenberg
```

- [ ] **Step 3: Update Previewer/CMakeLists.txt**

Add after line 13 (after `Render/TextureTransfer.cpp`):
```cmake
        Render/VideoPreviewer.cpp
        Render/VideoPreviewer.hpp
```

- [ ] **Step 4: Build and verify**

```bash
cd build && cmake --build . --target HeisenbergPreviewer
```
Expected: build succeeds. VideoPreviewer compiles and links.

- [ ] **Step 5: Commit**

```bash
git add Previewer/Render/VideoPreviewer.hpp Previewer/Render/VideoPreviewer.cpp Previewer/CMakeLists.txt
git commit -m "feat: add VideoPreviewer — pure C++ Vulkan render pipeline"
```

---

### Task 3: Refactor PlaybackController to emit AVFrame directly

**Files:**
- Modify: `Previewer/Controller/PlaybackController.hpp`
- Modify: `Previewer/Controller/PlaybackController.cpp`

**Interfaces:**
- Consumes: SoftwareDecoder (via IDecoder)
- Produces: `frameDecoded(std::shared_ptr<AVFrame>)` signal (replaces `frameReady(QImage)`)

- [ ] **Step 1: Modify PlaybackController.hpp**

Remove the `frameReady(QImage)` signal and `avFrameToQImage` method. Add `frameDecoded(shared_ptr<AVFrame>)`.

Replace:
```cpp
#include <QObject>
#include <QImage>
#include <memory>

// 前向声明 FFmpeg C 结构体（完整定义在 .cpp 中）
extern "C" {
struct AVFrame;
}
```

With:
```cpp
#include <QObject>
#include <memory>

// 前向声明 FFmpeg C 结构体（完整定义在 .cpp 中）
extern "C" {
struct AVFrame;
}
```

In the signals section, replace:
```cpp
    void frameReady(const QImage& image);
```
With:
```cpp
    void frameDecoded(std::shared_ptr<AVFrame> frame);
```

In the private section, remove:
```cpp
    QImage avFrameToQImage(const ::AVFrame* frame);
```

- [ ] **Step 2: Modify PlaybackController.cpp — remove includes**

Remove line 21:
```cpp
#include <libswscale/swscale.h>
```

- [ ] **Step 3: Modify PlaybackController.cpp — remove SwsContext from Impl**

In the `Impl` struct (lines ~56-61), remove:
```cpp
    // -- sws 颜色转换（缓存） --
    SwsContext* swsCtx = nullptr;
    int swsSrcW = 0;
    int swsSrcH = 0;
    AVPixelFormat swsSrcFmt = AV_PIX_FMT_NONE;
```

- [ ] **Step 4: Modify PlaybackController.cpp — update destructor**

Replace lines 76-81:
```cpp
PlaybackController::~PlaybackController() {
    close();
    if (impl_->swsCtx) {
        sws_freeContext(impl_->swsCtx);
        impl_->swsCtx = nullptr;
    }
}
```

With:
```cpp
PlaybackController::~PlaybackController() {
    close();
}
```

- [ ] **Step 5: Modify decodeFirstFrame() (lines 178-188)**

Replace:
```cpp
void PlaybackController::decodeFirstFrame() {
    auto frame = decodeFrameForTarget(std::numeric_limits<double>::max());
    if (frame) {
        impl_->lastDisplayedPtsMs = static_cast<double>(frame->pts);
        QImage img = avFrameToQImage(frame.get());
        if (!img.isNull()) {
            emit frameReady(img);
            emit positionChanged(frame->pts / 1000.0);
        }
    }
}
```

With:
```cpp
void PlaybackController::decodeFirstFrame() {
    auto frame = decodeFrameForTarget(std::numeric_limits<double>::max());
    if (frame) {
        impl_->lastDisplayedPtsMs = static_cast<double>(frame->pts);
        emit frameDecoded(frame);
        emit positionChanged(frame->pts / 1000.0);
    }
}
```

- [ ] **Step 6: Modify seek() (lines 240-249)**

Replace:
```cpp
    auto frame = decodeFrameForTarget(targetPtsMs);
    if (frame) {
        impl_->lastDisplayedPtsMs = static_cast<double>(frame->pts);
        QImage img = avFrameToQImage(frame.get());
        if (!img.isNull()) {
            emit frameReady(img);
            emit positionChanged(frame->pts / 1000.0);
        }
    }
```

With:
```cpp
    auto frame = decodeFrameForTarget(targetPtsMs);
    if (frame) {
        impl_->lastDisplayedPtsMs = static_cast<double>(frame->pts);
        emit frameDecoded(frame);
        emit positionChanged(frame->pts / 1000.0);
    }
```

- [ ] **Step 7: Modify onTick() (lines 295-304)**

Replace:
```cpp
    if (frameToDisplay) {
        double pts = static_cast<double>(frameToDisplay->pts);
        if (pts != impl_->lastDisplayedPtsMs) {
            impl_->lastDisplayedPtsMs = pts;
            QImage img = avFrameToQImage(frameToDisplay.get());
            if (!img.isNull()) {
                emit frameReady(img);
                emit positionChanged(pts / 1000.0);
            }
        }
    }
```

With:
```cpp
    if (frameToDisplay) {
        double pts = static_cast<double>(frameToDisplay->pts);
        if (pts != impl_->lastDisplayedPtsMs) {
            impl_->lastDisplayedPtsMs = pts;
            emit frameDecoded(frameToDisplay);
            emit positionChanged(pts / 1000.0);
        }
    }
```

- [ ] **Step 8: Remove avFrameToQImage() method entirely**

Delete lines 400-451 (the entire `avFrameToQImage` function).

- [ ] **Step 9: Build and verify**

```bash
cd build && cmake --build . --target MainApp
```
Expected: build succeeds. PlaybackController no longer depends on QImage or swscale.

- [ ] **Step 10: Commit**

```bash
git add Previewer/Controller/PlaybackController.hpp Previewer/Controller/PlaybackController.cpp
git commit -m "refactor: PlaybackController emits AVFrame directly, removes sws_scale/QImage"
```

---

### Task 4: Create VideoOutputItem (QQuickItem + child HWND)

**Files:**
- Create: `UserInterface/Backend/VideoOutputItem.hpp`
- Create: `UserInterface/Backend/VideoOutputItem.cpp`

**Interfaces:**
- Consumes: `QQuickItem`, `QQuickWindow` (Qt 6)
- Produces: `nativeWindowReady(HWND)`, `nativeWindowDestroyed()` signals

- [ ] **Step 1: Write VideoOutputItem.hpp**

```cpp
//
// Created by NiceFold on 2026/7/9.
//

#pragma once

#include <QQuickItem>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

/// VideoOutputItem — QML 中的 Vulkan 渲染目标
///
/// 在 QML 布局中占据一块区域，内部创建一个原生子 HWND 供 Vulkan 渲染。
/// 通过 WS_CLIPSIBLINGS 确保子窗口不覆盖 QML 兄弟控件。
class VideoOutputItem : public QQuickItem {
    Q_OBJECT

public:
    explicit VideoOutputItem(QQuickItem* parent = nullptr);
    ~VideoOutputItem() override;

    /// 对外暴露原生窗口句柄
    HWND nativeWindow() const { return hwnd_; }

signals:
    /// 子 HWND 创建完成，下游可调用 nativeWindow() 获取句柄
    void nativeWindowReady(HWND hwnd);
    /// 子 HWND 即将销毁
    void nativeWindowDestroyed();

protected:
    void componentComplete() override;
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;

private:
    void createChildWindow();
    void destroyChildWindow();
    void syncChildWindowGeometry();

    HWND hwnd_       = nullptr;
    bool  initialized_ = false;

    // 注册子窗口类（进程级一次性）
    static const wchar_t* windowClassName();
    static bool classRegistered_;
};
```

- [ ] **Step 2: Write VideoOutputItem.cpp**

```cpp
//
// Created by NiceFold on 2026/7/9.
//

#include "VideoOutputItem.hpp"

#include <QQuickWindow>
#include <stdexcept>

bool VideoOutputItem::classRegistered_ = false;

const wchar_t* VideoOutputItem::windowClassName() {
    return L"HeisenbergVideoOutput";
}

static LRESULT CALLBACK childWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

VideoOutputItem::VideoOutputItem(QQuickItem* parent)
    : QQuickItem(parent) {
    // 接受 hover 事件（可选，用于鼠标交互）
    setAcceptHoverEvents(false);
    setFlag(ItemHasContents, false); // 不由 QSGRenderer 绘制
}

VideoOutputItem::~VideoOutputItem() {
    destroyChildWindow();
}

void VideoOutputItem::componentComplete() {
    QQuickItem::componentComplete();

    QQuickWindow* w = window();
    if (w && w->isVisible()) {
        createChildWindow();
    }
    // 如果窗口尚不可见，等待 geometryChange 或下次 itemChange 触发
}

void VideoOutputItem::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) {
    QQuickItem::geometryChange(newGeometry, oldGeometry);

    // 首次有有效尺寸时创建子窗口
    if (!initialized_ && newGeometry.width() > 0 && newGeometry.height() > 0) {
        createChildWindow();
    }

    // 已创建子窗口 → 同步位置和大小
    if (hwnd_ && (newGeometry.width() != oldGeometry.width()
                  || newGeometry.height() != oldGeometry.height()
                  || newGeometry.x() != oldGeometry.x()
                  || newGeometry.y() != oldGeometry.y())) {
        syncChildWindowGeometry();
    }
}

void VideoOutputItem::createChildWindow() {
    if (hwnd_) return;

    QQuickWindow* w = window();
    if (!w) return;

    // 确保父窗口有原生句柄
    HWND parentHwnd = reinterpret_cast<HWND>(w->winId());
    if (!parentHwnd) return;

    // 注册窗口类（进程级一次性）
    if (!classRegistered_) {
        WNDCLASSEXW wc = {};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = childWindowProc;
        wc.hInstance     = GetModuleHandleW(nullptr);
        wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        wc.lpszClassName = windowClassName();
        wc.style         = CS_HREDRAW | CS_VREDRAW;

        if (!RegisterClassExW(&wc)) {
            return;
        }
        classRegistered_ = true;
    }

    // 创建子窗口
    QRectF geo = this->geometry();
    int ww = static_cast<int>(geo.width());
    int hh = static_cast<int>(geo.height());
    if (ww <= 0 || hh <= 0) return;

    hwnd_ = CreateWindowExW(
        0,
        windowClassName(),
        L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        0, 0, ww, hh,
        parentHwnd,
        nullptr,
        GetModuleHandleW(nullptr),
        nullptr);

    if (!hwnd_) return;

    syncChildWindowGeometry();
    initialized_ = true;

    emit nativeWindowReady(hwnd_);
}

void VideoOutputItem::destroyChildWindow() {
    if (hwnd_) {
        emit nativeWindowDestroyed();
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    initialized_ = false;
}

void VideoOutputItem::syncChildWindowGeometry() {
    if (!hwnd_) return;

    QQuickWindow* w = window();
    if (!w) return;

    // QQuickItem 本地坐标 → 场景坐标 → 窗口客户区坐标
    QPointF scenePos = mapToScene(QPointF(0, 0));
    int x = static_cast<int>(scenePos.x());
    int y = static_cast<int>(scenePos.y());
    int ww = static_cast<int>(width());
    int hh = static_cast<int>(height());

    if (ww <= 0 || hh <= 0) return;

    SetWindowPos(hwnd_, nullptr, x, y, ww, hh,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
}
```

- [ ] **Step 3: Build and verify**

```bash
cd build && cmake --build . --target MainApp
```
Expected: build succeeds. VideoOutputItem compiles with Qt/QML headers.

- [ ] **Step 4: Commit**

```bash
git add UserInterface/Backend/VideoOutputItem.hpp UserInterface/Backend/VideoOutputItem.cpp
git commit -m "feat: add VideoOutputItem — QQuickItem with native child HWND"
```

---

### Task 5: Refactor PlayerController to wire new pipeline

**Files:**
- Modify: `UserInterface/Backend/PlayerController.hpp`
- Modify: `UserInterface/Backend/PlayerController.cpp`

**Interfaces:**
- Consumes: `PlaybackController`, `VideoPreviewer`, `VideoOutputItem`
- Produces: `Q_INVOKABLE bindVideoOutput(VideoOutputItem*)`

- [ ] **Step 1: Modify PlayerController.hpp**

Replace the current content with:

```cpp
//
// Created by NiceFold on 2026/6/30.
//

#pragma once

#include <QObject>
#include <QString>
#include <memory>

struct AVFrame;

namespace heisenberg {
namespace ctrl   { class PlaybackController; }
namespace render { class VideoPreviewer; }
}

class VideoOutputItem;

namespace heisenberg {
namespace ui {

class PlayerController : public QObject {
    Q_OBJECT

    Q_PROPERTY(bool    isPlaying   READ isPlaying   NOTIFY isPlayingChanged)
    Q_PROPERTY(double  currentTime READ currentTime NOTIFY currentTimeChanged)
    Q_PROPERTY(double  duration    READ duration    NOTIFY durationChanged)
    Q_PROPERTY(bool    isSeekable  READ isSeekable  NOTIFY isSeekableChanged)
    Q_PROPERTY(QString currentFile READ currentFile NOTIFY currentFileChanged)

public:
    explicit PlayerController(QObject* parent = nullptr);
    ~PlayerController() override;

    bool    isPlaying()   const { return isPlaying_; }
    double  currentTime() const { return currentTime_; }
    double  duration()    const { return duration_; }
    bool    isSeekable()  const { return isSeekable_; }
    QString currentFile() const { return currentFile_; }

    void setPlaybackController(heisenberg::ctrl::PlaybackController* ctrl);

public slots:
    void play();
    void pause();
    void togglePlayPause();
    void seek(double seconds);
    void stepForward(int frames = 1);
    void stepBackward(int frames = 1);
    void goToStart();
    void goToEnd();

    Q_INVOKABLE bool openFile(const QString& path);
    Q_INVOKABLE void closeFile();

    /// QML 调用：绑定 VideoOutputItem（子 HWND 就绪后自动初始化管线）
    Q_INVOKABLE void bindVideoOutput(VideoOutputItem* item);

signals:
    void isPlayingChanged();
    void currentTimeChanged();
    void durationChanged();
    void isSeekableChanged();
    void currentFileChanged();

private slots:
    void onFrameDecoded(std::shared_ptr<AVFrame> frame);
    void onNativeWindowReady(HWND hwnd);

private:
    void disconnectPreviewer();

    bool    isPlaying_   = false;
    double  currentTime_ = 0.0;
    double  duration_    = 0.0;
    bool    isSeekable_  = false;
    QString currentFile_;

    heisenberg::ctrl::PlaybackController* ctrl_ = nullptr;
    std::unique_ptr<heisenberg::render::VideoPreviewer> previewer_;

    // 视频原始尺寸（首帧确定）
    int videoWidth_  = 0;
    int videoHeight_ = 0;
};

} // namespace ui
} // namespace heisenberg
```

- [ ] **Step 2: Rewrite PlayerController.cpp**

```cpp
//
// Created by NiceFold on 2026/6/30.
//

#include "PlayerController.hpp"

#include <Controller/PlaybackController.hpp>
#include <Render/VideoPreviewer.hpp>
#include <Backend/VideoOutputItem.hpp>
#include <Utiles/Logger.hpp>

extern "C" {
#include <libavutil/frame.h>
}

namespace heisenberg {
namespace ui {

PlayerController::PlayerController(QObject* parent)
    : QObject(parent) {
    previewer_ = std::make_unique<render::VideoPreviewer>();
}

PlayerController::~PlayerController() = default;

// ============================================================
// 依赖注入
// ============================================================

void PlayerController::setPlaybackController(ctrl::PlaybackController* ctrl) {
    ctrl_ = ctrl;
    if (!ctrl_) return;

    // frameDecoded → 渲染管线
    connect(ctrl_, &ctrl::PlaybackController::frameDecoded,
            this, &PlayerController::onFrameDecoded);

    // positionChanged → currentTime
    connect(ctrl_, &ctrl::PlaybackController::positionChanged,
            this, [this](double seconds) {
        currentTime_ = seconds;
        emit currentTimeChanged();
    });

    // stateChanged → isPlaying
    connect(ctrl_, &ctrl::PlaybackController::stateChanged,
            this, [this](ctrl::PlaybackController::State s) {
        bool playing = (s == ctrl::PlaybackController::Playing);
        if (isPlaying_ != playing) {
            isPlaying_ = playing;
            emit isPlayingChanged();
        }
    });

    // durationChanged
    connect(ctrl_, &ctrl::PlaybackController::durationChanged,
            this, [this](double d) {
        duration_ = d;
        emit durationChanged();
    });

    // endOfStream
    connect(ctrl_, &ctrl::PlaybackController::endOfStream,
            this, [this]() {
        if (isPlaying_) {
            isPlaying_ = false;
            emit isPlayingChanged();
        }
    });
}

// ============================================================
// VideoOutputItem 绑定
// ============================================================

void PlayerController::bindVideoOutput(VideoOutputItem* item) {
    if (!item) return;

    // 子 HWND 就绪 → 初始化管线
    connect(item, &VideoOutputItem::nativeWindowReady,
            this, &PlayerController::onNativeWindowReady,
            Qt::SingleShotConnection);

    // Item 尺寸变化 → 重建 swapchain
    connect(item, &QQuickItem::geometryChanged,
            this, [this, item]() {
        if (item->width() > 0 && item->height() > 0) {
            previewer_->resize(static_cast<int>(item->width()),
                               static_cast<int>(item->height()));
        }
    });

    // 如果已经就绪
    if (item->nativeWindow()) {
        onNativeWindowReady(item->nativeWindow());
    }
}

void PlayerController::onNativeWindowReady(HWND hwnd) {
    int w = videoWidth_  > 0 ? videoWidth_  : 640;
    int h = videoHeight_ > 0 ? videoHeight_ : 360;

    bool ok = previewer_->initialize(hwnd, w, h);
    if (!ok) {
        LOG_ERROR("PlayerController: VideoPreviewer::initialize() failed");
        return;
    }

    // resize 回调（视频分辨率变化时更新 UI）
    previewer_->setOnResize([this](int width, int height) {
        videoWidth_  = width;
        videoHeight_ = height;
    });

    LOG_INFO("PlayerController: Vulkan pipeline bound to HWND 0x{:x}", reinterpret_cast<uintptr_t>(hwnd));
}

void PlayerController::disconnectPreviewer() {
    // 预留：未来回收管线资源
}

// ============================================================
// 帧回调
// ============================================================

void PlayerController::onFrameDecoded(std::shared_ptr<AVFrame> frame) {
    if (!frame || !frame->data[0]) return;

    // 记录原始视频分辨率（首帧）
    if (videoWidth_ <= 0 || videoHeight_ <= 0) {
        videoWidth_  = frame->width;
        videoHeight_ = frame->height;
    }

    previewer_->presentFrame(frame.get());
}

// ============================================================
// 文件加载
// ============================================================

bool PlayerController::openFile(const QString& path) {
    if (!ctrl_) return false;

    bool ok = ctrl_->open(path.toStdString());
    if (ok) {
        currentFile_ = path;
        duration_    = ctrl_->duration();
        isSeekable_  = ctrl_->isSeekable();
        emit currentFileChanged();
        emit durationChanged();
        emit isSeekableChanged();
    }
    return ok;
}

void PlayerController::closeFile() {
    if (ctrl_) ctrl_->close();
    currentFile_.clear();
    currentTime_ = 0.0;
    duration_    = 0.0;
    isSeekable_  = false;
    isPlaying_   = false;
    videoWidth_  = 0;
    videoHeight_ = 0;
    emit currentFileChanged();
    emit currentTimeChanged();
    emit durationChanged();
    emit isSeekableChanged();
    emit isPlayingChanged();
}

// ============================================================
// 播放控制
// ============================================================

void PlayerController::play()             { if (ctrl_) ctrl_->play(); }
void PlayerController::pause()            { if (ctrl_) ctrl_->pause(); }
void PlayerController::togglePlayPause()  { if (ctrl_) ctrl_->togglePlayPause(); }
void PlayerController::seek(double s)     { if (ctrl_) ctrl_->seek(s); }
void PlayerController::stepForward(int n) { if (ctrl_) ctrl_->stepForward(n); }
void PlayerController::stepBackward(int n){ if (ctrl_) ctrl_->stepBackward(n); }
void PlayerController::goToStart()        { if (ctrl_) ctrl_->goToStart(); }
void PlayerController::goToEnd()          { if (ctrl_) ctrl_->goToEnd(); }

} // namespace ui
} // namespace heisenberg
```

- [ ] **Step 3: Build and verify**

```bash
cd build && cmake --build . --target MainApp
```
Expected: build succeeds. PlayerController no longer depends on FrameImageProvider.

- [ ] **Step 4: Commit**

```bash
git add UserInterface/Backend/PlayerController.hpp UserInterface/Backend/PlayerController.cpp
git commit -m "refactor: PlayerController wires VideoPreviewer + VideoOutputItem"
```

---

### Task 6: Update main.cpp

**Files:**
- Modify: `UserInterface/main.cpp`

**Interfaces:**
- Consumes: `VideoOutputItem` (register with QML), `PlayerController`
- Produces: running application

- [ ] **Step 1: Read main.cpp to confirm current state**

Current file is in `UserInterface/main.cpp`.

- [ ] **Step 2: Update main.cpp**

Remove the FrameImageProvider. Add VideoOutputItem registration.

Replace:
```cpp
#include <Backend/MediaPoolModel.hpp>
#include <Backend/PlayerController.hpp>
#include <Backend/MediaInfoProvider.hpp>
#include <Backend/ColorGradeModel.hpp>
#include <Backend/ScopeDataProvider.hpp>
#include <FrameImageProvider.hpp>
```

With:
```cpp
#include <Backend/MediaPoolModel.hpp>
#include <Backend/PlayerController.hpp>
#include <Backend/MediaInfoProvider.hpp>
#include <Backend/ColorGradeModel.hpp>
#include <Backend/ScopeDataProvider.hpp>
#include <Backend/VideoOutputItem.hpp>
```

Replace:
```cpp
    auto* imageProvider = new FrameImageProvider();

    heisenberg::ctrl::PlaybackController playbackCtrl;
    heisenberg::ui::PlayerController     playerCtrl;

    playerCtrl.setPlaybackController(&playbackCtrl);
    playerCtrl.setImageProvider(imageProvider);
```

With:
```cpp
    heisenberg::ctrl::PlaybackController playbackCtrl;
    heisenberg::ui::PlayerController     playerCtrl;

    playerCtrl.setPlaybackController(&playbackCtrl);
```

In the type registration section, add:
```cpp
    qmlRegisterType<VideoOutputItem>     ("Heisenberg", 1, 0, "VideoOutputItem");
```

Remove:
```cpp
    engine.addImageProvider("frame", imageProvider);
```

- [ ] **Step 3: Build and verify**

```bash
cd build && cmake --build . --target MainApp
```
Expected: build succeeds. No FrameImageProvider references remain.

- [ ] **Step 4: Commit**

```bash
git add UserInterface/main.cpp
git commit -m "refactor: main.cpp removes FrameImageProvider, registers VideoOutputItem"
```

---

### Task 7: Update ViewerPane.qml to use VideoOutputItem

**Files:**
- Modify: `UserInterface/Qml/ViewerPane.qml`

- [ ] **Step 1: Replace Image with VideoOutputItem**

Replace lines 28-59 (the `Item` containing `Image`):
```qml
        // ============================================
        // 视频画面区域
        // ============================================
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            // 通过 FrameImageProvider 显示解码帧
            // frameRevision 变化时 URL 变化，驱动 QML 重新请求帧
            Image {
                id: videoPreview
                anchors.centerIn: parent
                width: parent.width * 0.9
                height: parent.height * 0.9
                fillMode: Image.PreserveAspectFit
                source: "image://frame/preview?" + (root.controller ? root.controller.frameRevision : 0)
                cache: false

                // 无帧时显示占位文字
                Rectangle {
                    anchors.fill: parent
                    color: "#0a0a0a"
                    border.color: "#333333"
                    z: -1
                }

                Label {
                    anchors.centerIn: parent
                    text: videoPreview.sourceSize.width > 0
                          ? "" : "Video Preview"
                    color: "#444444"
                    font.pixelSize: 18
                }
            }
        }
```

With:
```qml
        // ============================================
        // 视频画面区域（Vulkan 渲染）
        // ============================================
        Item {
            id: videoContainer
            Layout.fillWidth: true
            Layout.fillHeight: true

            VideoOutputItem {
                id: videoOutput
                anchors.fill: parent

                Component.onCompleted: {
                    if (root.controller) {
                        root.controller.bindVideoOutput(videoOutput)
                    }
                }
            }

            // 无视频时显示占位文字
            Label {
                anchors.centerIn: parent
                text: root.controller && root.controller.currentFile
                      ? "" : "Video Preview"
                color: "#444444"
                font.pixelSize: 18
            }
        }
```

Ensure the import for `Heisenberg 1.0` is at the top of the QML file (it already is, at line 11).

- [ ] **Step 2: Build and verify**

```bash
cd build && cmake --build . --target MainApp
```
Expected: build succeeds. QML compiles with VideoOutputItem type.

- [ ] **Step 3: Commit**

```bash
git add UserInterface/Qml/ViewerPane.qml
git commit -m "refactor: ViewerPane uses VideoOutputItem for Vulkan rendering"
```

---

### Task 8: Update CMakeLists files

**Files:**
- Modify: `UserInterface/CMakeLists.txt`

- [ ] **Step 1: Add VideoOutputItem sources to UserInterface/CMakeLists.txt**

Replace the source list:
```cmake
add_executable(MainApp
    main.cpp
    qml.qrc
    Backend/MediaPoolModel.hpp        Backend/MediaPoolModel.cpp
    Backend/PlayerController.hpp      Backend/PlayerController.cpp
    Backend/MediaInfoProvider.hpp      Backend/MediaInfoProvider.cpp
    Backend/ColorGradeModel.hpp        Backend/ColorGradeModel.cpp
    Backend/ScopeDataProvider.hpp      Backend/ScopeDataProvider.cpp
    ../Previewer/Controller/PlaybackController.hpp
    ../Previewer/Controller/PlaybackController.cpp
)
```

With:
```cmake
add_executable(MainApp
    main.cpp
    qml.qrc
    Backend/MediaPoolModel.hpp        Backend/MediaPoolModel.cpp
    Backend/PlayerController.hpp      Backend/PlayerController.cpp
    Backend/MediaInfoProvider.hpp      Backend/MediaInfoProvider.cpp
    Backend/ColorGradeModel.hpp        Backend/ColorGradeModel.cpp
    Backend/ScopeDataProvider.hpp      Backend/ScopeDataProvider.cpp
    Backend/VideoOutputItem.hpp        Backend/VideoOutputItem.cpp
    ../Previewer/Controller/PlaybackController.hpp
    ../Previewer/Controller/PlaybackController.cpp
)
```

- [ ] **Step 2: Build and verify**

```bash
cd build && cmake --build . --target MainApp
```
Expected: build succeeds with all new files compiled and linked.

- [ ] **Step 3: Commit**

```bash
git add UserInterface/CMakeLists.txt
git commit -m "build: add VideoOutputItem to UserInterface CMakeLists"
```

---

### Task 9: Full build, verify, and integration test

- [ ] **Step 1: Clean rebuild**

```bash
cd build && cmake --build . --clean-first --target MainApp
```
Expected: clean build succeeds with no errors.

- [ ] **Step 2: Run the application**

```bash
./build/Debug/MainApp.exe path/to/test/video.mp4
```

- [ ] **Step 3: Verify basic rendering**

Check:
- Application window appears with QML layout intact
- VideoOutputItem creates a child HWND (visible as a black rectangle overlay)
- Playback controls remain visible and not covered
- Play/pause works and video frames render via Vulkan
- Resize the window — child HWND follows correctly
- Z-order: progress bar is on top of the video area

- [ ] **Step 4: Check logs for errors**

Verify no libplacebo/Vulkan errors in the console output. Expect to see:
```
Vulkan: Volk + Hpp dispatcher initialized
Vulkan: Instance created
Vulkan: selected discrete GPU — ...
Vulkan: Device created successfully
libplacebo: pl_log created
libplacebo: pl_vulkan imported
SwapChain: created — WxH
VideoPreviewer: initialized — WxH
```

- [ ] **Step 5: Fix any issues found during testing**

Common potential issues:
- Child HWND showing white instead of black: set background brush
- Vulkan surface creation failing: check device extensions
- RS_CLIPSIBLINGS not preventing overlap: adjust Z-order or parent clipping
- Video frames not visible: check that VideoPreviewer::presentFrame is called

- [ ] **Step 6: Final commit**

```bash
git add -A
git commit -m "chore: final adjustments for Vulkan rendering pipeline"
```
