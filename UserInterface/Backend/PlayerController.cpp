//
// Created by NiceFold on 2026/6/30.
//

#include "PlayerController.hpp"

#include <Controller/PlaybackController.hpp>
#include <IPreviewer.hpp>
#include <Renderer/GpuContext.hpp>
#include <Renderer/VulkanContext.hpp>
#include <Backend/VideoOutputItem.hpp>
#include <Utiles/Logger.hpp>

#include <QVulkanInstance>
#include <QQuickWindow>
#include <QSGRendererInterface>

extern "C" {
#include <libavutil/frame.h>
}

namespace heisenberg {
namespace ui {

PlayerController::PlayerController(QObject* parent)
    : QObject(parent) {
    previewer_ = std::make_unique<renderer::IPreviewer>();
}

PlayerController::~PlayerController() = default;

void PlayerController::shutdown() {
    if (shutdownDone_) return;
    shutdownDone_ = true;

    // 1. 先让 VideoOutputItem 排空缓存的 VkImage（通过回调安全销毁）
    if (videoOutput_) {
        videoOutput_->releaseTextureCache();
        videoOutput_->setVkImageDestroyCallback(nullptr);
    }

    // 2. 销毁 IPreviewer（内部 pl_tex / pl_vulkan_sem 等需要 pl_gpu 存活）
    previewer_.reset();

    // 3. 销毁 GpuContext（pl_vulkan_destroy 需要 Qt 的 VkDevice 存活）
    gpuCtx_.reset();

    LOG_INFO("PlayerController: GPU resources released");
}

// ============================================================
// 依赖注入
// ============================================================

void PlayerController::setPlaybackController(ctrl::PlaybackController* ctrl) {
    ctrl_ = ctrl;
    if (!ctrl_) return;

    // frameDecoded → 渲染管线
    connect(ctrl_, &ctrl::PlaybackController::frameDecoded,
            this, &PlayerController::onFrameDecoded);

    // positionChanged → currentTime
    connect(ctrl_, &ctrl::PlaybackController::positionChanged,
            this, [this](double seconds) {
        currentTime_ = seconds;
        emit currentTimeChanged();
    });

    // stateChanged → isPlaying
    connect(ctrl_, &ctrl::PlaybackController::stateChanged,
            this, [this](ctrl::PlaybackController::State s) {
        bool playing = (s == ctrl::PlaybackController::Playing);
        if (isPlaying_ != playing) {
            isPlaying_ = playing;
            emit isPlayingChanged();
        }
    });

    // durationChanged
    connect(ctrl_, &ctrl::PlaybackController::durationChanged,
            this, [this](double d) {
        duration_ = d;
        emit durationChanged();
    });

    // endOfStream
    connect(ctrl_, &ctrl::PlaybackController::endOfStream,
            this, [this]() {
        if (isPlaying_) {
            isPlaying_ = false;
            emit isPlayingChanged();
        }
        if (currentTime_ != duration_) {
            currentTime_ = duration_;
            emit currentTimeChanged();
        }
    });
}

// ============================================================
// VideoOutputItem 绑定 + 管线初始化
// ============================================================

void PlayerController::bindVideoOutput(VideoOutputItem* item) {
    if (!item) return;
    videoOutput_ = item;

    auto initPipeline = [this]() {
        QQuickWindow* qWindow = videoOutput_->window();
        if (!qWindow) {
            LOG_ERROR("PlayerController: VideoOutputItem has no QQuickWindow");
            return;
        }

        QSGRendererInterface* ri = qWindow->rendererInterface();
        LOG_INFO("PlayerController: rendererInterface={}, graphicsApi={} (Vulkan={})",
                 reinterpret_cast<void*>(ri),
                 ri ? static_cast<int>(ri->graphicsApi()) : -1,
                 static_cast<int>(QSGRendererInterface::Vulkan));

        if (!ri || ri->graphicsApi() != QSGRendererInterface::Vulkan) {
            LOG_ERROR("PlayerController: QSGRendererInterface not available or not Vulkan");
            return;
        }

        // ---- 查询 Qt 的 Vulkan 资源 ----
        // VulkanInstanceResource 返回 QVulkanInstance*（不是 VkInstance*）
        auto* qVulkanInst = static_cast<QVulkanInstance*>(
            ri->getResource(qWindow, QSGRendererInterface::VulkanInstanceResource));
        auto* physDevicePtr = static_cast<VkPhysicalDevice*>(
            ri->getResource(qWindow, QSGRendererInterface::PhysicalDeviceResource));
        auto* devicePtr = static_cast<VkDevice*>(
            ri->getResource(qWindow, QSGRendererInterface::DeviceResource));
        auto* qfIndexPtr = static_cast<uint32_t*>(
            ri->getResource(qWindow, QSGRendererInterface::GraphicsQueueFamilyIndexResource));
        auto* queuePtr = static_cast<VkQueue*>(
            ri->getResource(qWindow, QSGRendererInterface::CommandQueueResource));

        if (!qVulkanInst || !physDevicePtr || !devicePtr || !qfIndexPtr || !queuePtr) {
            LOG_ERROR("PlayerController: failed to query Vulkan resources from Qt");
            return;
        }

        VkInstance       instance   = qVulkanInst->vkInstance();
        VkPhysicalDevice physDevice = *physDevicePtr;
        VkDevice         device     = *devicePtr;
        VkQueue          queue      = *queuePtr;
        uint32_t         qfIndex    = *qfIndexPtr;

        LOG_INFO("PlayerController: Vulkan resources — instance={}, physDev={}, device={}, "
                 "qfIndex={}, queue={}",
                 reinterpret_cast<void*>(instance),
                 reinterpret_cast<void*>(physDevice),
                 reinterpret_cast<void*>(device),
                 qfIndex,
                 reinterpret_cast<void*>(queue));

        LOG_INFO("PlayerController: Qt Vulkan — instance=0x{:x}, device=0x{:x}, QF={}",
                 reinterpret_cast<uintptr_t>(instance),
                 reinterpret_cast<uintptr_t>(device),
                 qfIndex);

        // ---- 加载 Qt 的 Vulkan 句柄到 Volk ----
        auto& vkCtx = renderer::VulkanContext::instance();
        vkCtx.initInstance(instance);
        vkCtx.initDevice(device);

        // ---- 构建 GpuContext 所需资源 ----
        renderer::VulkanResources vkRes;
        vkRes.instance       = instance;
        vkRes.physDevice     = physDevice;
        vkRes.device         = device;
        vkRes.graphicsQF     = qfIndex;
        vkRes.graphicsQueue  = queue;
        vkRes.getProcAddr    = vkCtx.getInstanceProcAddr();

        // ---- 创建 GpuContext（扩展枚举在 GpuContext 内部完成）----
        try {
            gpuCtx_ = std::make_unique<renderer::GpuContext>(vkRes);
        } catch (const std::exception& e) {
            LOG_ERROR("PlayerController: GpuContext creation failed — {}", e.what());
            return;
        }

        // ---- 初始化 IPreviewer ----
        int w = videoWidth_  > 0 ? videoWidth_  : 640;
        int h = videoHeight_ > 0 ? videoHeight_ : 360;
        bool ok = previewer_->initialize(gpuCtx_->gpuContext(), vkRes.device,
                                         vkRes.graphicsQF, w, h);
        if (!ok) {
            LOG_ERROR("PlayerController: IPreviewer::initialize() failed");
            gpuCtx_.reset();
            return;
        }

        // ---- 注入 VkImage 销毁回调 ----
        // VideoOutputItem 在 QSGTexture 被替换后回调 IPreviewer 销毁旧 vk::Image
        videoOutput_->setVkImageDestroyCallback([this](vk::Image img) {
            previewer_->destroyVkImage(img);
        });

        previewer_->setOnResize([this](int width, int height) {
            videoWidth_  = width;
            videoHeight_ = height;
        });

        // ---- 尺寸变化 → 更新输出分辨率 ----
        auto syncSize = [this]() {
            qreal sw = videoOutput_->width();
            qreal sh = videoOutput_->height();
            if (sw > 0 && sh > 0) {
                previewer_->resize(static_cast<int>(sw), static_cast<int>(sh));
            }
        };
        connect(videoOutput_, &QQuickItem::widthChanged,  this, syncSize);
        connect(videoOutput_, &QQuickItem::heightChanged, this, syncSize);
        // 初始尺寸
        syncSize();

        LOG_INFO("PlayerController: Vulkan pipeline initialized and bound to VideoOutputItem");

        // ---- 注册场景图销毁回调（官方互操作标准做法） ----
        // sceneGraphInvalidated 触发时 QRhi 仍存活，必须用 DirectConnection 同步执行
        connect(qWindow, &QQuickWindow::sceneGraphInvalidated, this, [this]() {
            LOG_INFO("PlayerController: scene graph invalidating, releasing GPU resources...");
            if (ctrl_) ctrl_->pause();
            shutdown();
        }, Qt::DirectConnection);
    };

    // 等待场景图就绪后再初始化管线（Vulkan 资源在场景图初始化后才可用）
    QQuickWindow* qWindow = item->window();
    if (qWindow && qWindow->isSceneGraphInitialized()) {
        initPipeline();
    } else if (qWindow) {
        connect(qWindow, &QQuickWindow::sceneGraphInitialized,
                this, initPipeline, Qt::SingleShotConnection);
    } else {
        LOG_ERROR("PlayerController: VideoOutputItem has no QQuickWindow");
    }
}

// ============================================================
// 帧回调
// ============================================================

void PlayerController::onFrameDecoded(std::shared_ptr<AVFrame> frame) {
    if (!frame || !frame->data[0]) return;

    if (videoWidth_ <= 0 || videoHeight_ <= 0) {
        videoWidth_  = frame->width;
        videoHeight_ = frame->height;
        LOG_INFO("PlayerController: first frame decoded — {}x{}", videoWidth_, videoHeight_);
    }

    // 渲染 → 导出 VkImage
    auto output = previewer_->presentFrame(frame.get());
    if (!output.image) {
        LOG_WARN("PlayerController: presentFrame returned null VkImage");
        return;
    }

    // 提交给 VideoOutputItem 显示
    if (videoOutput_) {
        videoOutput_->presentVkImage(output.image, output.layout,
                                      output.width, output.height);
    }
}

// ============================================================
// 文件加载
// ============================================================

bool PlayerController::openFile(const QString& path) {
    if (!ctrl_) return false;

    bool ok = ctrl_->open(path.toStdString());
    if (ok) {
        currentFile_ = path;
        emit currentFileChanged();
        // duration_ / isSeekable_ 由异步回调链更新，不在此处读取
    }
    return ok;
}

void PlayerController::closeFile() {
    if (ctrl_) ctrl_->close();
    currentFile_.clear();
    currentTime_ = 0.0;
    duration_    = 0.0;
    isSeekable_  = false;
    isPlaying_   = false;
    videoWidth_  = 0;
    videoHeight_ = 0;
    emit currentFileChanged();
    emit currentTimeChanged();
    emit durationChanged();
    emit isSeekableChanged();
    emit isPlayingChanged();
}

// ============================================================
// 播放控制
// ============================================================

void PlayerController::play()             { if (ctrl_) ctrl_->play(); }
void PlayerController::pause()            { if (ctrl_) ctrl_->pause(); }
void PlayerController::togglePlayPause()  { if (ctrl_) ctrl_->togglePlayPause(); }
void PlayerController::seek(double s)     { if (ctrl_) ctrl_->seek(s); }
void PlayerController::stepForward(int n) { if (ctrl_) ctrl_->stepForward(n); }
void PlayerController::stepBackward(int n){ if (ctrl_) ctrl_->stepBackward(n); }
void PlayerController::goToStart()        { if (ctrl_) ctrl_->goToStart(); }
void PlayerController::goToEnd()          { if (ctrl_) ctrl_->goToEnd(); }

} // namespace ui
} // namespace heisenberg
