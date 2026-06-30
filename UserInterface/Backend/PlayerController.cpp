//
// Created by NiceFold on 2026/6/30.
//

#include "PlayerController.hpp"

namespace heisenberg {
namespace ui {

PlayerController::PlayerController(QObject* parent)
    : QObject(parent)
{
    // TODO: Phase 2 — 创建 DecoderWrapper，连接帧到达信号
}

PlayerController::~PlayerController() = default;

void PlayerController::play()
{
    // TODO: Phase 2 — 启动解码线程 + 渲染定时器
}

void PlayerController::pause()
{
    // TODO: Phase 2 — 暂停渲染定时器
}

void PlayerController::togglePlayPause()
{
    // TODO: Phase 2
}

void PlayerController::seek(double /*seconds*/)
{
    // TODO: Phase 2 — Demuxer::seek + Decoder::flush + 逐帧定位
}

void PlayerController::stepForward(int /*frames*/)
{
    // TODO: Phase 2
}

void PlayerController::stepBackward(int /*frames*/)
{
    // TODO: Phase 2
}

void PlayerController::goToStart()
{
    // TODO: Phase 2
}

void PlayerController::goToEnd()
{
    // TODO: Phase 2
}

} // namespace ui
} // namespace heisenberg
