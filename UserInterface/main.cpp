#include <QApplication>
#include <QFileInfo>
#include <QString>
#include <Backend/MediaInfoProbe.hpp>
#include <MainWidget/MainWindow.hpp>
#include <Backend/PlayerController.hpp>
#include <Backend/EditorSession.hpp>
#include <Backend/QtExportController.hpp>
#include <Preview/IPreviewer.hpp>
#include <Utiles/Logger.hpp>

int main(int argc, char* argv[]) {
    heisenberg::IPreviewer::initLoader();

    QApplication app(argc, argv);
    app.setApplicationName("Heisenberg");
    qRegisterMetaType<heisenberg::ui::MediaInfoReport>();

    heisenberg::Logger::Init();

    heisenberg::ui::EditorSession editorSession;
    heisenberg::ui::PlayerController playerCtrl(&editorSession);
    playerCtrl.setHardwareDecode(true);

    MainWindow mainWindow(&editorSession);
    heisenberg::ui::QtExportController exportCtrl(&editorSession, &playerCtrl);
    QObject::connect(&mainWindow, &MainWindow::exportTimelineRequested, &exportCtrl,
                     [&]() { exportCtrl.chooseAndExport(&mainWindow); });
    QObject::connect(&exportCtrl, &heisenberg::ui::QtExportController::finished,
                     &mainWindow, [&](bool, const QString& message) { mainWindow.showError(message); });
    QObject::connect(&mainWindow, &MainWindow::aboutToClose, &exportCtrl,
                     &heisenberg::ui::QtExportController::shutdown, Qt::DirectConnection);
    QObject::connect(&mainWindow, &MainWindow::aboutToClose,
                     &playerCtrl, &heisenberg::ui::PlayerController::shutdown,
                     Qt::DirectConnection);

    playerCtrl.bindVideoOutput(mainWindow.videoWidget());

    QObject::connect(&mainWindow, &MainWindow::playPauseClicked,
                     &playerCtrl, &heisenberg::ui::PlayerController::togglePlayPause);
    QObject::connect(&mainWindow, &MainWindow::scrubStarted,
                     &playerCtrl, &heisenberg::ui::PlayerController::beginScrub);
    QObject::connect(&mainWindow, &MainWindow::scrubFrameRequested,
                     &playerCtrl, &heisenberg::ui::PlayerController::scrubToFrame);
    QObject::connect(&mainWindow, &MainWindow::scrubFinished,
                     &playerCtrl, &heisenberg::ui::PlayerController::endScrub);
    QObject::connect(&mainWindow, &MainWindow::sourceSeekRequested,
                     &playerCtrl, &heisenberg::ui::PlayerController::seek);
    QObject::connect(&mainWindow, &MainWindow::openFileRequested,
                     &playerCtrl, &heisenberg::ui::PlayerController::openFile);
    QObject::connect(&mainWindow, &MainWindow::previewMediaRequested,
                     &playerCtrl, &heisenberg::ui::PlayerController::previewAsset);
    QObject::connect(&mainWindow, &MainWindow::timelinePreviewRequested,
                     &playerCtrl, [&]() { playerCtrl.showTimeline(); });
    QObject::connect(&playerCtrl, &heisenberg::ui::PlayerController::previewModeChanged,
                     &mainWindow, &MainWindow::setSourcePreview);
    QObject::connect(&playerCtrl, &heisenberg::ui::PlayerController::playbackFailed,
                     &mainWindow, &MainWindow::showError);
    QObject::connect(&mainWindow, &MainWindow::openFilterGraphRequested,
                     &playerCtrl, &heisenberg::ui::PlayerController::openFilterGraph);
    QObject::connect(&mainWindow, &MainWindow::hardwareDecodeToggled,
                     &playerCtrl, &heisenberg::ui::PlayerController::setHardwareDecode);
    QObject::connect(&playerCtrl, &heisenberg::ui::PlayerController::filterGraphPathChanged,
                     &mainWindow, [&]() {
                         mainWindow.setFilterGraphPath(playerCtrl.filterGraphPath());
                     });
    QObject::connect(&playerCtrl, &heisenberg::ui::PlayerController::filterGraphLoadFailed,
                     &mainWindow, &MainWindow::setFilterGraphError);

    QObject::connect(&playerCtrl, &heisenberg::ui::PlayerController::currentFileChanged,
                     &mainWindow, [&]() {
                         if (playerCtrl.previewingSource())
                             mainWindow.showFileInfo(playerCtrl.currentFile());
                     });
    QObject::connect(&playerCtrl, &heisenberg::ui::PlayerController::isPlayingChanged,
                     &mainWindow, [&]() { mainWindow.setPlayingState(playerCtrl.isPlaying()); });
    QObject::connect(&playerCtrl, &heisenberg::ui::PlayerController::currentTimeChanged,
                     &mainWindow, [&]() { mainWindow.setCurrentTime(playerCtrl.currentTime()); });
    QObject::connect(&playerCtrl, &heisenberg::ui::PlayerController::durationChanged,
                     &mainWindow, [&]() { mainWindow.setDuration(playerCtrl.duration()); });
    QObject::connect(&playerCtrl, &heisenberg::ui::PlayerController::frameCountChanged,
                     &mainWindow, [&]() { mainWindow.setFrameCount(playerCtrl.frameCount()); });

    mainWindow.show();

    if (argc >= 2) {
        const QString path = QString::fromLocal8Bit(argv[1]);
        const QFileInfo info(path);
        if (info.isDir() && QFileInfo(info.filePath() + "/project.json").exists()) {
            mainWindow.loadProjectPackage(path, false);
        } else {
            playerCtrl.openFile(path);
        }
    }

    LOG_INFO("Heisenberg started");

    int ret = app.exec();

    exportCtrl.shutdown();
    playerCtrl.shutdown();
    heisenberg::Logger::Shutdown();
    return ret;
}
