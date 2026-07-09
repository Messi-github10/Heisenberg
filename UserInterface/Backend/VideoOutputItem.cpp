//
// Created by NiceFold on 2026/7/9.
//

#include "VideoOutputItem.hpp"

#include <QQuickWindow>
#include <Utiles/Logger.hpp>

bool VideoOutputItem::classRegistered_ = false;

const wchar_t* VideoOutputItem::windowClassName() {
    return L"HeisenbergVideoOutput";
}

static LRESULT CALLBACK popupWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_MOUSEACTIVATE) {
        return MA_NOACTIVATE;  // 不让 popup 抢焦点
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

VideoOutputItem::VideoOutputItem(QQuickItem* parent)
    : QQuickItem(parent) {
    setAcceptHoverEvents(false);
    setFlag(ItemHasContents, false);
}

VideoOutputItem::~VideoOutputItem() {
    destroyPopupWindow();
}

void VideoOutputItem::componentComplete() {
    QQuickItem::componentComplete();
    LOG_INFO("VideoOutputItem: componentComplete, size={}x{}", width(), height());
    if (width() > 0 && height() > 0) {
        createPopupWindow();
    } else {
        LOG_WARN("VideoOutputItem: zero size at componentComplete, deferring");
    }
    trackMainWindow();
}

void VideoOutputItem::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) {
    QQuickItem::geometryChange(newGeometry, oldGeometry);

    if (!initialized_ && newGeometry.width() > 0 && newGeometry.height() > 0) {
        createPopupWindow();
    }

    if (hwnd_ && (newGeometry.width()  != oldGeometry.width()
               || newGeometry.height() != oldGeometry.height()
               || newGeometry.x()      != oldGeometry.x()
               || newGeometry.y()      != oldGeometry.y())) {
        syncPopupGeometry();
    }
}

void VideoOutputItem::createPopupWindow() {
    if (hwnd_) return;

    QQuickWindow* w = window();
    if (!w) return;

    int ww = static_cast<int>(width());
    int hh = static_cast<int>(height());
    if (ww <= 0 || hh <= 0) return;

    if (!classRegistered_) {
        WNDCLASSEXW wc = {};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = popupWndProc;
        wc.hInstance     = GetModuleHandleW(nullptr);
        wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        wc.lpszClassName = windowClassName();
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        if (!RegisterClassExW(&wc)) return;
        classRegistered_ = true;
    }

    // 先创建独立 popup（无 owner），避免启动时干扰主窗口焦点
    HWND ownerHwnd = reinterpret_cast<HWND>(w->winId());
    hwnd_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        windowClassName(),
        L"",
        WS_POPUP,
        0, 0, ww, hh,
        nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr);

    if (!hwnd_) return;

    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    syncPopupGeometry();

    // 延迟绑定 owner：等主窗口完全就绪后再建立 owned 关系
    QMetaObject::invokeMethod(this, [this, ownerHwnd]() {
        if (hwnd_) {
            SetWindowLongPtr(hwnd_, GWLP_HWNDPARENT,
                             reinterpret_cast<LONG_PTR>(ownerHwnd));
        }
    }, Qt::QueuedConnection);

    initialized_ = true;
    LOG_INFO("VideoOutputItem: popup created, hwnd=0x{:x}, size={}x{}",
             reinterpret_cast<uintptr_t>(hwnd_), ww, hh);
    emit nativeWindowReady(hwnd_);
}

void VideoOutputItem::destroyPopupWindow() {
    if (hwnd_) {
        emit nativeWindowDestroyed();
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    initialized_ = false;
}

void VideoOutputItem::syncPopupGeometry() {
    if (!hwnd_) return;

    QQuickWindow* w = window();
    if (!w) return;

    QPointF scenePos = mapToScene(QPointF(0, 0));
    QPoint globalPos = w->mapToGlobal(scenePos.toPoint());

    int x = globalPos.x();
    int y = globalPos.y();
    int ww = static_cast<int>(width());
    int hh = static_cast<int>(height());
    if (ww <= 0 || hh <= 0) return;

    // 只移动/缩放，不改 Z-order（Z-order 在 activeChanged 显示时处理）
    SetWindowPos(hwnd_, nullptr, x, y, ww, hh,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
}

void VideoOutputItem::trackMainWindow() {
    QQuickWindow* w = window();
    if (!w) return;

    connect(w, &QQuickWindow::xChanged, this, [this]() { syncPopupGeometry(); });
    connect(w, &QQuickWindow::yChanged, this, [this]() { syncPopupGeometry(); });
    connect(w, &QQuickWindow::widthChanged, this, [this]() { syncPopupGeometry(); });
    connect(w, &QQuickWindow::heightChanged, this, [this]() { syncPopupGeometry(); });

}
