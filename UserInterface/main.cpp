//
// Created by NiceFold on 2026/6/30.
//

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QUrl>

// Backend 注册
#include <Backend/MediaPoolModel.hpp>
#include <Backend/PlayerController.hpp>
#include <Backend/MediaInfoProvider.hpp>
#include <Backend/ColorGradeModel.hpp>
#include <Backend/ScopeDataProvider.hpp>

int main(int argc, char* argv[])
{
    QGuiApplication app(argc, argv);
    app.setApplicationName("Heisenberg");

    // 使用 Fusion 样式以支持 background/contentItem 自定义
    QQuickStyle::setStyle("Fusion");

    // 注册 C++ 类型到 QML
    qmlRegisterType<heisenberg::ui::MediaPoolModel>    ("Heisenberg", 1, 0, "MediaPoolModel");
    qmlRegisterType<heisenberg::ui::PlayerController>   ("Heisenberg", 1, 0, "PlayerController");
    qmlRegisterType<heisenberg::ui::MediaInfoProvider>  ("Heisenberg", 1, 0, "MediaInfoProvider");
    qmlRegisterType<heisenberg::ui::ColorGradeModel>    ("Heisenberg", 1, 0, "ColorGradeModel");
    qmlRegisterType<heisenberg::ui::ScopeDataProvider>  ("Heisenberg", 1, 0, "ScopeDataProvider");

    QQmlApplicationEngine engine;
    engine.load(QUrl("qrc:/Qml/Main.qml"));

    if (engine.rootObjects().isEmpty()) {
        return -1;
    }

    return app.exec();
}
