//
// Created by NiceFold on 2026/7/9.
//

#pragma once

#include <QQuickItem>

#include <vulkan/vulkan.hpp>

#include <functional>
#include <mutex>

/// VideoOutputItem — QML 中的 Vulkan 渲染目标
///
/// 通过 QSG 场景图直接显示 VkImage，不再使用 WS_POPUP 弹窗。
/// VkImage 销毁通过回调委托给 Previewer 层（本类不直接调 Vulkan 函数）。
class VideoOutputItem : public QQuickItem {
    Q_OBJECT

public:
    using VkImageDestroyCallback = std::function<void(vk::Image)>;

    explicit VideoOutputItem(QQuickItem* parent = nullptr);
    ~VideoOutputItem() override;

    /// 主线程调用：提交一帧新的 vk::Image 用于显示
    Q_INVOKABLE void presentVkImage(vk::Image image, vk::ImageLayout layout,
                                     int width, int height);

    /// 设置 VkImage 销毁回调（由 PlayerController 在绑定时注入）
    void setVkImageDestroyCallback(VkImageDestroyCallback cb) { destroyCb_ = std::move(cb); }

    /// 主动释放缓存的 vk::Image（在场景图失效前由 PlayerController 调用）
    void releaseTextureCache();

protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) override;
    void releaseResources() override;

private:
    struct Frame {
        vk::Image      image;
        vk::ImageLayout layout = vk::ImageLayout::eUndefined;
        int            width  = 0;
        int            height = 0;
    };

    std::mutex mutex_;
    Frame      pendingFrame_;
    bool       hasPending_   = false;

    VkImageDestroyCallback destroyCb_;
    vk::Image currentVkImage_;  // 当前正被 QSGTexture 包装的 vk::Image
};
