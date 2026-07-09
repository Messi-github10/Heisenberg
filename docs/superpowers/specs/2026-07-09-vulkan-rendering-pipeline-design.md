# Vulkan Rendering Pipeline — Design Spec

**Date:** 2026-07-09
**Status:** Approved

## Goal

Replace the current CPU rendering path (sws_scale → QImage → FrameImageProvider → QML Image) with a GPU rendering pipeline: FFmpeg software decode → libplacebo (Vulkan) → native child HWND embedded in QML layout.

## Architecture Overview

```
FFmpeg Demuxer → SoftwareDecoder → AVFrame (CPU)
                                       ↓
                              PlaybackController: decode + clock only
                              emit frameDecoded(shared_ptr<AVFrame>)
                                       ↓
                              PlayerController: glue layer
                                       ↓
                              VideoPreviewer (pure C++, no Qt)
                              ├─ TextureTransfer::uploadAVFrame()   ← moved to render/
                              ├─ RenderEngine::render()            ← GPU YUV→RGB
                              └─ SwapChain → VkSurfaceKHR          ← renders to child HWND
                                       ↓
                              VideoOutputItem (QQuickItem)
                              ├─ creates WS_CHILD HWND via CreateWindowEx
                              └─ syncs geometry with QML layout
```

## New Components

### 1. VideoOutputItem (`UserInterface/Backend/VideoOutputItem.hpp/.cpp`)

`QQuickItem` subclass placed in QML layout where `Image` currently sits. This is a UI-layer component (depends on Qt), separate from the pure-C++ Previewer render pipeline.

**Responsibilities:**
- Create a native child HWND (`WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS`) via `CreateWindowEx` in `componentComplete()`
- Sync child HWND position/size to QQuickItem geometry in `geometryChange()`
- Emit `nativeWindowReady(HWND)` signal once child window is created
- Emit `nativeWindowDestroyed()` signal before destruction

**Key points:**
- Parent HWND obtained from `QQuickWindow::winId()`
- `WS_CLIPSIBLINGS` prevents child HWND drawing over sibling QML controls (progress bar, etc.)
- Coordinate mapping: QQuickItem local → scene → global → parent client coordinates

### 2. VideoPreviewer (`Previewer/Render/VideoPreviewer.hpp/.cpp`)

Pure C++ class in `heisenberg::render` namespace. No Qt dependencies.

**Interface:**
```cpp
class VideoPreviewer {
public:
    using ResizeCallback = std::function<void(int width, int height)>;
    using PresentCallback = std::function<void()>;

    bool initialize(HWND hwnd, int width, int height);
    bool presentFrame(AVFrame* avframe);
    void resize(int width, int height);
    void setOnResize(ResizeCallback cb);
    void setOnPresent(PresentCallback cb);
};
```

**Internal pipeline (per `presentFrame` call):**
1. `SwapChain::startFrame()` — begin frame
2. `TextureTransfer::uploadAVFrame(avframe)` — AVFrame → `pl_frame*` (GPU textures)
3. `RenderEngine::render(pl_frame, swapchain_target)` — GPU-side YUV→RGB via libplacebo
4. `SwapChain::submitFrame()` — submit frame

**Held resources:**
- `GpuContext` — wraps `pl_vulkan`
- `SwapChain` — wraps `pl_swapchain` with `VkSurfaceKHR` from child HWND
- `RenderEngine` — wraps `pl_renderer`
- `TextureTransfer` — AVFrame → pl_frame converter

### 3. TextureTransfer relocation

Moved from `Previewer/Decoder/TextureTransfer.hpp` to `Previewer/Render/TextureTransfer.hpp`.

Namespace changed from `heisenberg::decoder` to `heisenberg::render`.

## Modified Components

### 4. PlaybackController (`Previewer/Controller/PlaybackController.hpp/.cpp`)

**Removed:**
- `frameReady(QImage)` signal
- `avFrameToQImage()` method
- `SwsContext` caching and `sws_scale` usage
- `<libswscale/swscale.h>` include

**Added:**
- `frameDecoded(std::shared_ptr<AVFrame>)` signal — emits decoded AVFrame directly

**Changed callsites** (onTick, seek, decodeFirstFrame):
```cpp
// Before:
QImage img = avFrameToQImage(frame.get());
emit frameReady(img);

// After:
emit frameDecoded(frame);
```

### 5. PlayerController (`UserInterface/Backend/PlayerController.hpp/.cpp`)

**Removed:**
- `FrameImageProvider*` and `setImageProvider()`
- `frameRevision` property and `frameDecoded()` signal
- `QImage` includes

**Added:**
- `VideoPreviewer previewer_` member
- `setVideoOutputItem(VideoOutputItem*)` or `bindVideoOutput(VideoOutputItem*)` — Q_INVOKABLE
- `onFrameDecoded(shared_ptr<AVFrame>)` slot → `previewer_.presentFrame()`
- `onNativeWindowReady(HWND)` slot → `previewer_.initialize()`

**Wiring:**
1. QML `VideoOutputItem` ready → `nativeWindowReady(HWND)` → PlayerController → `previewer_.initialize(hwnd, w, h)`
2. `PlaybackController::frameDecoded` → PlayerController → `previewer_.presentFrame(frame)`
3. First frame → `onResize` callback → update VideoOutputItem implicit size

### 6. main.cpp (`UserInterface/main.cpp`)

**Removed:**
- `FrameImageProvider` instantiation and registration
- `setImageProvider()` call
- `engine.addImageProvider("frame", imageProvider)`

**Added:**
- `qmlRegisterType<VideoOutputItem>("Heisenberg", 1, 0, "VideoOutputItem")`

### 7. ViewerPane.qml (`UserInterface/Qml/ViewerPane.qml`)

**Replace:**
```qml
Image {
    source: "image://frame/preview?" + controller.frameRevision
    cache: false
    ...
}
```

**With:**
```qml
VideoOutputItem {
    id: videoOutput
    anchors.centerIn: parent
    Component.onCompleted: {
        root.controller.bindVideoOutput(videoOutput)
    }
}
```

### 8. CMakeLists.txt changes

**Previewer/CMakeLists.txt:**
- Add `Render/VideoPreviewer.hpp/.cpp`
- Relocate `TextureTransfer.hpp/.cpp` from Decoder to Render source list

**UserInterface/CMakeLists.txt:**
- Add `Backend/VideoOutputItem.hpp/.cpp`
- Remove `FrameImageProvider.hpp` from sources
- Link `HeisenbergPreviewer` already covers the dependency

## Data Flow Summary

```
┌─────────────────────────────────────────────────────────┐
│ QML ViewerPane                                          │
│   VideoOutputItem (QQuickItem) ──creates──→ child HWND  │
│        │                                                │
│   UI layout: anchors.centerIn, implicit size from video  │
└────────┼────────────────────────────────────────────────┘
         │ nativeWindowReady(HWND)
         ↓
PlayerController
    │                                    ┌──────────────────────┐
    ├─→ VideoPreviewer::initialize() ──→ │ GpuContext           │
    │                                    │   pl_vulkan          │
    │                                    │ SwapChain            │
    │                                    │   VkSurfaceKHR(HWND) │
    │                                    │ RenderEngine         │
    │                                    │   pl_renderer        │
    │                                    │ TextureTransfer      │
    │                                    │   AVFrame → pl_frame │
    │                                    └──────────────────────┘
    │                                              ↑
    │  PlaybackController::frameDecoded(AVFrame) ──┘
    │  → VideoPreviewer::presentFrame()
    │
    │  VideoPreviewer::onResize → VideoOutputItem::setImplicitSize()
    │  VideoPreviewer::onPresent → (future frame timing)
```

## Error Handling

- `VideoPreviewer::initialize()` returns false if GpuContext/SwapChain/RenderEngine creation fails; PlayerController logs and stays in "no video" state
- `VideoPreviewer::presentFrame()` returns false if any pipeline step fails; caller logs but playback continues (next frame may succeed after e.g. swapchain recreation)
- `VideoOutputItem` handles parent window destruction gracefully via `nativeWindowDestroyed` signal

## Things NOT in Scope

- Hardware decoding (D3D11, CUDA) — stubs remain, not wired
- Audio playback — not addressed
- Performance optimization / zero-copy — correctness first
