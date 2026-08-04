//
// Created by NiceFold on 2026/6/30.
//

#include <QApplication>

#include <MainWidget/MainWindow.hpp>
#include <MainWidget/VideoWidget.hpp>
#include <Backend/PlayerController.hpp>

#include <Controller/PlaybackController.hpp>
#include <Renderer/VulkanContext.hpp>

#include <Utiles/Logger.hpp>

#include <string>

int main(int argc, char* argv[])
{
    // ============================================================
    // 0. Volk 加载器（必须在任何 Vulkan 调用之前）
    // ============================================================
    heisenberg::renderer::VulkanContext::instance().initLoader();

    QApplication app(argc, argv);
    app.setApplicationName("Heisenberg");

    heisenberg::Logger::Init();

    // ============================================================
    // 1. Vulkan 上下文（独立创建 Instance + Device）
    // ============================================================
    try {
        heisenberg::renderer::VulkanContext::instance().createInstance();
        heisenberg::renderer::VulkanContext::instance().createDevice();
    } catch (const std::exception& e) {
        LOG_CRITICAL("Vulkan init failed: {}", e.what());
        heisenberg::Logger::Shutdown();
        return -1;
    }

    // ============================================================
    // 2. 核心对象
    // ============================================================
    heisenberg::ctrl::PlaybackController playbackCtrl;
    heisenberg::ui::PlayerController     playerCtrl;
    playerCtrl.setPlaybackController(&playbackCtrl);

    // ============================================================
    // 3. UI
    // ============================================================
    MainWindow mainWindow;

    // 绑定视频输出
    playerCtrl.bindVideoOutput(mainWindow.videoWidget());

    // 连接控制信号
    QObject::connect(&mainWindow, &MainWindow::playPauseClicked,
                     &playerCtrl, &heisenberg::ui::PlayerController::togglePlayPause);
    QObject::connect(&mainWindow, &MainWindow::scrubStarted,
                     &playerCtrl, &heisenberg::ui::PlayerController::beginScrub);
    QObject::connect(&mainWindow, &MainWindow::scrubFrameRequested,
                     &playerCtrl, &heisenberg::ui::PlayerController::scrubToFrame);
    QObject::connect(&mainWindow, &MainWindow::scrubFinished,
                     &playerCtrl, &heisenberg::ui::PlayerController::endScrub);
    QObject::connect(&mainWindow, &MainWindow::openFileRequested,
                     &playerCtrl, &heisenberg::ui::PlayerController::openFile);

    // 回写 UI 状态
    QObject::connect(&playerCtrl, &heisenberg::ui::PlayerController::isPlayingChanged,
                     &mainWindow, [&]() { mainWindow.setPlayingState(playerCtrl.isPlaying()); });
    QObject::connect(&playerCtrl, &heisenberg::ui::PlayerController::currentTimeChanged,
                     &mainWindow, [&]() { mainWindow.setCurrentTime(playerCtrl.currentTime()); });
    QObject::connect(&playerCtrl, &heisenberg::ui::PlayerController::durationChanged,
                     &mainWindow, [&]() { mainWindow.setDuration(playerCtrl.duration()); });
    QObject::connect(&playerCtrl, &heisenberg::ui::PlayerController::frameCountChanged,
                     &mainWindow, [&]() { mainWindow.setFrameCount(playerCtrl.frameCount()); });

    mainWindow.show();

    // ============================================================
    // 4. 命令行视频
    // ============================================================
    if (argc >= 2) {
        playerCtrl.openFile(QString::fromStdString(argv[1]));
    }

    LOG_INFO("Heisenberg started");

    int ret = app.exec();

    // ============================================================
    // 5. 清理
    // ============================================================
    playerCtrl.shutdown();
    heisenberg::Logger::Shutdown();
    return ret;
}
