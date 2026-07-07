//
// Created by NiceFold on 2026/7/7.
//
// FrameImageProvider — 将解码帧 QImage 传给 QML Image
//

#pragma once

#include <QQuickImageProvider>
#include <QImage>

class FrameImageProvider : public QQuickImageProvider {
public:
    FrameImageProvider()
        : QQuickImageProvider(QQuickImageProvider::Image) {}

    QImage requestImage(const QString& /*id*/, QSize* size,
                        const QSize& /*requestedSize*/) override
    {
        if (size)
            *size = frame_.size();
        return frame_;
    }

    void setFrame(const QImage& img) { frame_ = img; }

private:
    QImage frame_;
};
