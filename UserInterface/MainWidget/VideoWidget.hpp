//
// Created by NiceFold on 2026/7/15.
//

#pragma once

#include <QWidget>

class VideoWidget : public QWidget {
    Q_OBJECT

public:
    explicit VideoWidget(QWidget* parent = nullptr);
    ~VideoWidget() override;

    /// 返回内嵌 WS_CHILD 子窗口的 HWND
    HWND nativeWindow() const { return hwnd_; }

signals:
    void windowResized(int width, int height);

protected:
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    void createNativeWindow();
    void syncNativeWindow();

    HWND hwnd_     = nullptr;
    HWND parentHwnd_ = nullptr;
};

inline HWND getHwnd(VideoWidget* w) { return w ? w->nativeWindow() : nullptr; }
