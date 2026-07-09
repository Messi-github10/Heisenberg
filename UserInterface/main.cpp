//
// Created by NiceFold on 2026/6/30.
//

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QUrl>

// Backend 注册
#include <Backend/MediaPoolModel.hpp>
#include <Backend/PlayerController.hpp>
#include <Backend/MediaInfoProvider.hpp>
#include <Backend/ColorGradeModel.hpp>
#include <Backend/ScopeDataProvider.hpp>
#include <Backend/VideoOutputItem.hpp>

// Core 播放引擎
#include <Controller/PlaybackController.hpp>

#include <Utiles/Logger.hpp>

#include <string>

// ============================================================
// Main
// ============================================================
int main(int argc, char* argv[])
{
    QGuiApplication app(argc, argv);
    app.setApplicationName("Heisenberg");

    // 使用 Fusion 样式以支持 background/contentItem 自定义
    QQuickStyle::setStyle("Fusion");

    // 初始化日志
    heisenberg::Logger::Init();

    // ============================================================
    // 1. 创建核心对象
    // ============================================================
    heisenberg::ctrl::PlaybackController playbackCtrl;    // 播放引擎
    heisenberg::ui::PlayerController     playerCtrl;       // QML 胶水

    playerCtrl.setPlaybackController(&playbackCtrl);

    // ============================================================
    // 2. 注册 C++ 类型到 QML
    // ============================================================
    qmlRegisterType<heisenberg::ui::MediaPoolModel>   ("Heisenberg", 1, 0, "MediaPoolModel");
    qmlRegisterType<heisenberg::ui::MediaInfoProvider> ("Heisenberg", 1, 0, "MediaInfoProvider");
    qmlRegisterType<heisenberg::ui::ColorGradeModel>   ("Heisenberg", 1, 0, "ColorGradeModel");
    qmlRegisterType<heisenberg::ui::ScopeDataProvider> ("Heisenberg", 1, 0, "ScopeDataProvider");
    qmlRegisterType<VideoOutputItem>     ("Heisenberg", 1, 0, "VideoOutputItem");
    // PlayerController 不再注册为 QML 类型 — 通过 context property 注入

    // ============================================================
    // 3. QML 引擎
    // ============================================================
    QQmlApplicationEngine engine;

    engine.rootContext()->setContextProperty("playerController", &playerCtrl);

    engine.load(QUrl("qrc:/Qml/Main.qml"));

    if (engine.rootObjects().isEmpty()) {
        heisenberg::Logger::Shutdown();
        return -1;
    }

    // 查找 VideoOutputItem 并绑定到 PlayerController
    {
        auto* rootObj = engine.rootObjects().first();
        LOG_INFO("Main: root object type = {}", rootObj->metaObject()->className());
        auto* videoOutput = rootObj->findChild<VideoOutputItem*>("videoOutput");
        if (videoOutput) {
            LOG_INFO("Main: found VideoOutputItem, binding...");
            playerCtrl.bindVideoOutput(videoOutput);
        } else {
            LOG_ERROR("Main: VideoOutputItem NOT found! Searching all children...");
            auto children = rootObj->findChildren<QObject*>();
            for (auto* c : children) {
                LOG_INFO("Main: child: {} [{}]",
                         c->objectName().toStdString(), c->metaObject()->className());
            }
        }
    }

    // ============================================================
    // 4. 命令行视频路径（可选）
    // ============================================================
    if (argc >= 2) {
        playerCtrl.openFile(QString::fromStdString(argv[1]));
        LOG_INFO("Loaded video from command line: {}", argv[1]);
    }

    LOG_INFO("Heisenberg started successfully");

    int ret = app.exec();

    heisenberg::Logger::Shutdown();
    return ret;
}
