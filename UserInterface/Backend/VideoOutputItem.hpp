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
/// 创建 WS_POPUP 独立窗口覆盖在 QML 视频区域上，供 Vulkan 渲染。
class VideoOutputItem : public QQuickItem {
    Q_OBJECT

public:
    explicit VideoOutputItem(QQuickItem* parent = nullptr);
    ~VideoOutputItem() override;

    HWND nativeWindow() const { return hwnd_; }

signals:
    void nativeWindowReady(HWND hwnd);
    void nativeWindowDestroyed();

protected:
    void componentComplete() override;
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;

private:
    void createPopupWindow();
    void destroyPopupWindow();
    void syncPopupGeometry();
    void trackMainWindow();

    HWND hwnd_ = nullptr;
    bool initialized_ = false;

    static const wchar_t* windowClassName();
    static bool classRegistered_;
};
