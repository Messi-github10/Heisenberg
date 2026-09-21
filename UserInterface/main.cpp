#include <QApplication>
#include <QString>

#include <MainWidget/MainWindow.hpp>
#include <Backend/PlayerController.hpp>

#include <IPreviewer.hpp>
#include <Utiles/Logger.hpp>

int main(int argc, char* argv[])
{
    heisenberg::IPreviewer::initLoader();

    QApplication app(argc, argv);
    app.setApplicationName("Heisenberg");

    heisenberg::Logger::Init();

    heisenberg::ui::PlayerController playerCtrl;
    playerCtrl.setHardwareDecode(true);

    MainWindow mainWindow;

    // Stop playback and release GPU resources while the native video window
    // is still alive (MainWindow emits this before the base close handling).
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
    QObject::connect(&mainWindow, &MainWindow::openFileRequested,
                     &playerCtrl, &heisenberg::ui::PlayerController::openFile);
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
        playerCtrl.openFile(QString::fromStdString(argv[1]));
    }

    LOG_INFO("Heisenberg started");

    int ret = app.exec();

    playerCtrl.shutdown();
    heisenberg::Logger::Shutdown();
    return ret;
}
