//
// Created by NiceFold on 2026/7/15.
//

#include "MainWidget/VideoWidget.hpp"
#include <QResizeEvent>
#include <windows.h>
#include <Utiles/Logger.hpp>

static const wchar_t* kVideoWindowClass = L"HeisenbergVideoChild";

VideoWidget::VideoWidget(QWidget* parent)
    : QWidget(parent) {
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, Qt::black);
    setPalette(pal);
    createNativeWindow();
}

VideoWidget::~VideoWidget() {
    if (hwnd_) {
        DestroyWindow(hwnd_);
    }
}

void VideoWidget::createNativeWindow() {
    // 注册子窗口类（全局只注册一次）
    static bool classRegistered = false;
    if (!classRegistered) {
        WNDCLASSEXW wc = {};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = DefWindowProcW;
        wc.hInstance     = GetModuleHandle(nullptr);
        wc.lpszClassName = kVideoWindowClass;
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        RegisterClassExW(&wc);
        classRegistered = true;
    }

    parentHwnd_ = reinterpret_cast<HWND>(winId());

    hwnd_ = CreateWindowExW(
        0,                          // dwExStyle
        kVideoWindowClass,          // class name
        L"",                        // window name
        WS_CHILD | WS_VISIBLE,      // style
        0, 0, width(), height(),    // initial size
        parentHwnd_,                // parent
        nullptr,                    // menu
        GetModuleHandle(nullptr),   // instance
        nullptr                     // param
    );

    if (!hwnd_) {
        LOG_ERROR("VideoWidget: CreateWindowEx failed");
    } else {
        LOG_INFO("VideoWidget: native child window created — HWND=0x{:x}",
                 reinterpret_cast<uintptr_t>(hwnd_));
    }
}

void VideoWidget::syncNativeWindow() {
    if (!hwnd_) return;
    SetWindowPos(hwnd_, nullptr,
                 0, 0, width(), height(),
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

void VideoWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    syncNativeWindow();
    emit windowResized(event->size().width(), event->size().height());
}

void VideoWidget::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (hwnd_) {
        ShowWindow(hwnd_, SW_SHOW);
        syncNativeWindow();
    }
}
