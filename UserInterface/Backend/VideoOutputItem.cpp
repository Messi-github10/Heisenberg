//
// Created by NiceFold on 2026/7/9.
//

#include "VideoOutputItem.hpp"

#include <QQuickWindow>
#include <QSGSimpleTextureNode>
#include <QSGTexture>
#include <qsgtexture_platform.h>    // QNativeInterface::QSGVulkanTexture

#include <Utiles/Logger.hpp>

VideoOutputItem::VideoOutputItem(QQuickItem* parent)
    : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
    setAcceptHoverEvents(false);
}

VideoOutputItem::~VideoOutputItem() {
    // 销毁当前显示的 VkImage（如果有）
    if (currentVkImage_ && destroyCb_) {
        destroyCb_(currentVkImage_);
    }
    // 销毁待显示的 VkImage（如果有）
    if (hasPending_ && pendingFrame_.image && destroyCb_) {
        destroyCb_(pendingFrame_.image);
    }
}

void VideoOutputItem::presentVkImage(vk::Image image, vk::ImageLayout layout,
                                      int width, int height) {
    if (!image) return;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        // 上一帧 pending 未被消费（场景图来不及渲染），直接销毁
        if (hasPending_ && pendingFrame_.image && destroyCb_) {
            destroyCb_(pendingFrame_.image);
        }
        pendingFrame_ = { image, layout, width, height };
        hasPending_   = true;
    }

    update();  // 触发场景图重绘 → updatePaintNode
}

QSGNode* VideoOutputItem::updatePaintNode(QSGNode* oldNode,
                                           UpdatePaintNodeData*) {
    Frame frame;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (hasPending_) {
            frame       = pendingFrame_;
            hasPending_ = false;
        }
    }

    // 没有待显示帧 → 保留旧节点
    if (!frame.image) {
        return oldNode;
    }

    auto* node = static_cast<QSGSimpleTextureNode*>(oldNode);
    if (!node) {
        node = new QSGSimpleTextureNode();
        node->setFiltering(QSGTexture::Linear);
        node->setOwnsTexture(true);  // Qt 场景图接管 QSGTexture 生命周期
    }

    QQuickWindow* w = window();
    if (w) {
        QSGTexture* tex = QNativeInterface::QSGVulkanTexture::fromNative(
            static_cast<VkImage>(frame.image),
            static_cast<VkImageLayout>(frame.layout),
            w,
            QSize(frame.width, frame.height),
            QQuickWindow::TextureHasAlphaChannel);

        if (tex) {
            node->setTexture(tex);
            // setOwnsTexture(true) 保证 setTexture 时旧 QSGTexture 被 node 自动 delete，
            // 此时才能安全销毁旧 VkImage
            if (currentVkImage_ && destroyCb_) {
                destroyCb_(currentVkImage_);
            }
            currentVkImage_ = frame.image;
        }
    }

    node->setRect(boundingRect());
    return node;
}

void VideoOutputItem::releaseTextureCache() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (currentVkImage_ && destroyCb_) {
        destroyCb_(currentVkImage_);
        currentVkImage_ = nullptr;
    }
    if (hasPending_ && pendingFrame_.image && destroyCb_) {
        destroyCb_(pendingFrame_.image);
        pendingFrame_ = {};
        hasPending_   = false;
    }
}

void VideoOutputItem::releaseResources() {
    // 先让 Qt 销毁场景图 Node 树 → setOwnsTexture(true) 触发 QSGTexture delete
    QQuickItem::releaseResources();
    // 再清洗我们缓存的 VkImage 句柄
    releaseTextureCache();
}
