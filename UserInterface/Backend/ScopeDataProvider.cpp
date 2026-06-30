//
// Created by NiceFold on 2026/6/30.
//

#include "ScopeDataProvider.hpp"

namespace heisenberg {
namespace ui {

ScopeDataProvider::ScopeDataProvider(QObject* parent)
    : QObject(parent)
{
}

ScopeDataProvider::~ScopeDataProvider() = default;

// TODO: Phase 2 — 实现示波器数据计算
//       - void updateFromFrame(const AVFrame* frame) — 每帧调用
//       - 内部用 QImage 绘制示波器位图，QML 直接显示

} // namespace ui
} // namespace heisenberg
