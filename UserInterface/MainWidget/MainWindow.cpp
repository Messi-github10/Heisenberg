//
// Created by NiceFold on 2026/7/15.
//

#include "MainWidget/MainWindow.hpp"
#include "MainWidget/TimelineWidget.hpp"
#include "MainWidget/VideoWidget.hpp"

#include <QAction>
#include <QCloseEvent>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSplitter>
#include <QStatusBar>
#include <QStyle>
#include <QToolBar>
#include <QWidget>

#include <algorithm>
#include <cmath>

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent) {
    setWindowTitle("Heisenberg");
    resize(1200, 760);
    setMinimumSize(760, 500);
    setupUi();
    setupConnections();
}

void MainWindow::closeEvent(QCloseEvent* event) {
    emit aboutToClose();
    QMainWindow::closeEvent(event);
}

void MainWindow::setupUi() {
    setStyleSheet(R"(
        QMainWindow, QMenuBar, QMenu, QToolBar, QStatusBar {
            background: #18191c;
            color: rgba(255, 255, 255, 0.90);
        }
        QMenuBar::item:selected, QMenu::item:selected { background: #303137; }
        QToolBar { border: 0; border-bottom: 1px solid #08090a; spacing: 3px; padding: 3px 6px; }
        QToolButton { border: 0; padding: 5px; min-width: 24px; min-height: 24px; }
        QToolButton:hover { background: #303137; }
        QToolButton:pressed, QToolButton:checked { background: #26272c; }
        QSplitter::handle { background: #08090a; height: 4px; }
        QStatusBar { border-top: 1px solid rgba(255, 255, 255, 0.08); }
    )");

    auto* fileMenu = menuBar()->addMenu(tr("File"));
    auto* openProjectAction = fileMenu->addAction(
        style()->standardIcon(QStyle::SP_DirOpenIcon), tr("Open Palmier Project..."));
    openProjectAction->setShortcut(QKeySequence::Open);
    auto* openVideoAction = fileMenu->addAction(
        style()->standardIcon(QStyle::SP_FileIcon), tr("Open Video..."));
    auto* openGraphAction = fileMenu->addAction(tr("Open Filter Graph..."));

    auto* playbackMenu = menuBar()->addMenu(tr("Playback"));
    playPauseAction_ = playbackMenu->addAction(
        style()->standardIcon(QStyle::SP_MediaPlay), tr("Play / Pause"));
    playPauseAction_->setShortcut(Qt::Key_Space);
    hardwareDecodeAction_ = playbackMenu->addAction(tr("Hardware Decode"));
    hardwareDecodeAction_->setCheckable(true);
    hardwareDecodeAction_->setChecked(true);

    auto* timelineMenu = menuBar()->addMenu(tr("Timeline"));
    auto* zoomOutAction = timelineMenu->addAction(tr("Zoom Out"));
    zoomOutAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Minus));
    auto* zoomInAction = timelineMenu->addAction(tr("Zoom In"));
    zoomInAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Plus));
    auto* fitAction = timelineMenu->addAction(tr("Fit Timeline"));
    fitAction->setShortcut(QKeySequence(Qt::Key_F));

    auto* toolbar = addToolBar(tr("Editor"));
    toolbar->setMovable(false);
    toolbar->setFloatable(false);
    toolbar->setIconSize(QSize(18, 18));
    toolbar->addAction(openProjectAction);
    toolbar->addAction(openVideoAction);
    toolbar->addSeparator();
    toolbar->addAction(playPauseAction_);

    auto* leftSpacer = new QWidget(toolbar);
    leftSpacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar->addWidget(leftSpacer);

    timelineTitleLabel_ = new QLabel(tr("Timeline"), toolbar);
    timelineTitleLabel_->setStyleSheet(
        "font-weight: 600; color: rgba(255, 255, 255, 0.90); padding: 0 8px;");
    toolbar->addWidget(timelineTitleLabel_);
    timeLabel_ = new QLabel("00:00 / 00:00", toolbar);
    timeLabel_->setMinimumWidth(112);
    timeLabel_->setAlignment(Qt::AlignCenter);
    timeLabel_->setStyleSheet(
        "font-family: Consolas, monospace; color: #f29933; padding: 0 8px;");
    toolbar->addWidget(timeLabel_);

    auto* rightSpacer = new QWidget(toolbar);
    rightSpacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar->addWidget(rightSpacer);
    auto* toolbarZoomOut = toolbar->addAction("-");
    toolbarZoomOut->setToolTip(tr("Zoom Out"));
    auto* toolbarFit = toolbar->addAction(tr("Fit"));
    toolbarFit->setToolTip(tr("Fit Timeline"));
    auto* toolbarZoomIn = toolbar->addAction("+");
    toolbarZoomIn->setToolTip(tr("Zoom In"));

    auto* splitter = new QSplitter(Qt::Vertical, this);
    splitter->setChildrenCollapsible(false);
    splitter->setHandleWidth(4);
    videoWidget_ = new VideoWidget(splitter);
    videoWidget_->setMinimumSize(320, 180);
    timelineWidget_ = new TimelineWidget(splitter);
    splitter->addWidget(videoWidget_);
    splitter->addWidget(timelineWidget_);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 0);
    splitter->setSizes({520, 200});
    setCentralWidget(splitter);
    statusBar()->setSizeGripEnabled(false);

    connect(openProjectAction, &QAction::triggered, this, [this]() {
        const QString path = QFileDialog::getExistingDirectory(
            this, tr("Open Palmier Project Package"), QString(),
            QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
        if (!path.isEmpty() && loadProjectPackage(path)) emit openPlaylistRequested(path);
    });
    connect(openVideoAction, &QAction::triggered, this, [this]() {
        const QString path = QFileDialog::getOpenFileName(
            this, tr("Open Video"), QString(),
            tr("Video Files (*.mp4 *.mkv *.avi *.mov *.webm);;All Files (*)"));
        if (!path.isEmpty()) {
            timelineWidget_->clearProject();
            timelineTitleLabel_->setText(QFileInfo(path).completeBaseName());
            emit openFileRequested(path);
        }
    });
    connect(openGraphAction, &QAction::triggered, this, [this]() {
        const QString path = QFileDialog::getOpenFileName(
            this, tr("Open Filter Graph"), QString(),
            tr("Filter Graphs (*.json);;All Files (*)"));
        if (!path.isEmpty()) emit openFilterGraphRequested(path);
    });
    connect(zoomOutAction, &QAction::triggered, timelineWidget_, &TimelineWidget::zoomOut);
    connect(zoomInAction, &QAction::triggered, timelineWidget_, &TimelineWidget::zoomIn);
    connect(fitAction, &QAction::triggered, timelineWidget_, &TimelineWidget::fitToContent);
    connect(toolbarZoomOut, &QAction::triggered, timelineWidget_, &TimelineWidget::zoomOut);
    connect(toolbarZoomIn, &QAction::triggered, timelineWidget_, &TimelineWidget::zoomIn);
    connect(toolbarFit, &QAction::triggered, timelineWidget_, &TimelineWidget::fitToContent);
}

void MainWindow::setupConnections() {
    connect(playPauseAction_, &QAction::triggered, this, &MainWindow::playPauseClicked);
    connect(hardwareDecodeAction_, &QAction::toggled, this, &MainWindow::hardwareDecodeToggled);
    connect(timelineWidget_, &TimelineWidget::scrubStarted, this, &MainWindow::scrubStarted);
    connect(timelineWidget_, &TimelineWidget::scrubFrameRequested,
            this, &MainWindow::scrubFrameRequested);
    connect(timelineWidget_, &TimelineWidget::scrubFinished, this, &MainWindow::scrubFinished);
}

bool MainWindow::hardwareDecodeEnabled() const {
    return hardwareDecodeAction_ && hardwareDecodeAction_->isChecked();
}

bool MainWindow::loadProjectPackage(const QString& path, bool showError) {
    QString error;
    if (!timelineWidget_->loadPackage(path, &error)) {
        if (showError) QMessageBox::critical(this, tr("Cannot Open Project"), error);
        return false;
    }
    timelineTitleLabel_->setText(timelineWidget_->timelineName());
    setWindowTitle(QString("%1 - Heisenberg").arg(QFileInfo(path).completeBaseName()));
    statusBar()->showMessage(tr("Loaded %1").arg(QDir::toNativeSeparators(path)), 3500);
    return true;
}

void MainWindow::setDuration(double seconds) {
    duration_ = std::max(0.0, seconds);
}

void MainWindow::setFrameCount(qint64 frameCount) {
    frameCount_ = std::max<qint64>(0, frameCount);
    timelineWidget_->setFrameCount(frameCount_);
}

void MainWindow::setCurrentTime(double seconds) {
    if (duration_ > 0.0 && frameCount_ > 0) {
        const double normalized = std::clamp(seconds / duration_, 0.0, 1.0);
        const qint64 frame = std::clamp<qint64>(
            static_cast<qint64>(std::llround(normalized * static_cast<double>(frameCount_))),
            0, frameCount_ - 1);
        timelineWidget_->setCurrentFrame(frame);
    }

    const auto format = [](double value) {
        const int total = std::max(0, static_cast<int>(value));
        return QString("%1:%2")
            .arg(total / 60, 2, 10, QChar('0'))
            .arg(total % 60, 2, 10, QChar('0'));
    };
    timeLabel_->setText(format(seconds) + " / " + format(duration_));
}

void MainWindow::setPlayingState(bool playing) {
    isPlaying_ = playing;
    playPauseAction_->setIcon(style()->standardIcon(
        playing ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));
    playPauseAction_->setToolTip(playing ? tr("Pause") : tr("Play"));
    timelineWidget_->setPlaying(playing);
}

void MainWindow::setFilterGraphPath(const QString& path) {
    statusBar()->showMessage(tr("Filter graph: %1").arg(QFileInfo(path).fileName()), 3500);
}

void MainWindow::setFilterGraphError(const QString& message) {
    statusBar()->showMessage(tr("Filter graph failed: %1").arg(message), 6000);
}
