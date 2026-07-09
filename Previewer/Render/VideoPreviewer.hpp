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
