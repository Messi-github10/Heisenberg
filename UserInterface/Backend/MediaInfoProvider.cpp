//
// Created by NiceFold on 2026/6/30.
//

#include "MediaInfoProvider.hpp"

namespace heisenberg {
namespace ui {

MediaInfoProvider::MediaInfoProvider(QObject* parent)
    : QObject(parent)
{
}

MediaInfoProvider::~MediaInfoProvider() = default;

// TODO: Phase 2 — 添加 void updateFromStream(const Stream&) 方法
//       当用户在 MediaPool 中选择文件时调用

} // namespace ui
} // namespace heisenberg
