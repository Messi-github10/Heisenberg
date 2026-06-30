//
// Created by NiceFold on 2026/6/30.
//

#pragma once

#include <QObject>
#include <QImage>

namespace heisenberg {
namespace ui {

class ScopeDataProvider : public QObject {
    Q_OBJECT

    Q_PROPERTY(QImage waveform    READ waveform    NOTIFY scopesChanged)
    Q_PROPERTY(QImage vectorscope READ vectorscope NOTIFY scopesChanged)
    Q_PROPERTY(QImage histogram   READ histogram   NOTIFY scopesChanged)

public:
    explicit ScopeDataProvider(QObject* parent = nullptr);
    ~ScopeDataProvider() override;

    QImage waveform()    const { return waveform_; }
    QImage vectorscope() const { return vectorscope_; }
    QImage histogram()   const { return histogram_; }

signals:
    void scopesChanged();

private:
    QImage waveform_;
    QImage vectorscope_;
    QImage histogram_;

    // TODO: Phase 2 — 从当前帧 AVFrame 数据计算示波器图像
    //       - Waveform:   逐列亮度分布
    //       - Vectorscope: U/V 分量散点图
    //       - Histogram:   RGB 通道直方图
};

} // namespace ui
} // namespace heisenberg
