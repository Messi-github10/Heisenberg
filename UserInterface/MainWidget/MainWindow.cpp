//
// Created by NiceFold on 2026/7/15.
//

#include "MainWidget/MainWindow.hpp"
#include "MainWidget/TimelineWidget.hpp"
#include "MainWidget/VideoWidget.hpp"
#include "MainWidget/MediaPoolWidget.hpp"
#include "MainWidget/MediaInfoPanel.hpp"
#include "MainWidget/RoundedPanel.hpp"
#include "Backend/EditorSession.hpp"
#include "Backend/MediaInfoProbe.hpp"

#include <QAction>
#include <QCloseEvent>
#include <QFutureWatcher>
#include <QtConcurrent>
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
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

MainWindow::MainWindow(heisenberg::ui::EditorSession* session, QWidget* parent)
    : QMainWindow(parent), session_(session) {
    setWindowTitle("Heisenberg");
    resize(1200, 760);
    setMinimumSize(760, 500);
    setupUi();
    setupConnections();
    updateProjectTitle();
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (!confirmDiscardChanges()) { event->ignore(); return; }
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
        QSplitter::handle { background: #121316; }
        QStatusBar { border-top: 1px solid rgba(255, 255, 255, 0.08); }
        QWidget#workspace, QWidget#editorSplit { background: #121316; }
        QWidget#mediaPool, QWidget#mediaInfo, QWidget#previewPanel { background: transparent; color: #ececef; }
        QWidget#mediaPool QLabel, QWidget#previewPanel QLabel, QWidget#mediaInfo QLabel { color: #d8d9df; }
        QListWidget { background: transparent; border: 0; outline: 0; }
        QLineEdit { background: #18191c; border: 1px solid #37383e; border-radius: 4px;
                    padding: 6px; color: #ececef; }
        QPushButton { background: #303137; border: 1px solid #41434a; border-radius: 4px;
                      padding: 5px 8px; color: #ececef; }
        QPushButton:hover { background: #3b3d44; }
        QScrollBar:vertical { background: transparent; width: 10px; }
        QScrollBar::handle:vertical { background: #494b53; min-height: 24px; border-radius: 4px; }
    )");

    auto* fileMenu = menuBar()->addMenu(tr("File"));
    auto* openProjectAction = fileMenu->addAction(
        style()->standardIcon(QStyle::SP_DirOpenIcon), tr("Open Palmier Project..."));
    openProjectAction->setShortcut(QKeySequence::Open);
    auto* openVideoAction = fileMenu->addAction(
        style()->standardIcon(QStyle::SP_FileIcon), tr("Open Video..."));
    auto* openGraphAction = fileMenu->addAction(tr("Open Filter Graph..."));
    auto* importAction = fileMenu->addAction(tr("Import Media..."));
    importAction->setShortcut(QKeySequence("Ctrl+I"));
    fileMenu->addSeparator();
    auto* saveAction = fileMenu->addAction(tr("Save Project"));
    saveAction->setShortcut(QKeySequence::Save);
    auto* saveAsAction = fileMenu->addAction(tr("Save Project As..."));
    saveAsAction->setShortcut(QKeySequence::SaveAs);

    auto* editMenu = menuBar()->addMenu(tr("Edit"));
    auto* undoAction = editMenu->addAction(tr("Undo"));
    undoAction->setShortcut(QKeySequence::Undo);
    auto* redoAction = editMenu->addAction(tr("Redo"));
    redoAction->setShortcuts({QKeySequence::Redo, QKeySequence("Ctrl+Shift+Z")});
    auto* splitAction = editMenu->addAction(tr("Split at Playhead"));
    splitAction->setObjectName("splitAtPlayhead");
    splitAction->setToolTip(tr("Select a clip, place the playhead inside it, then split (S when the timeline has focus)."));

    auto* playbackMenu = menuBar()->addMenu(tr("Playback"));
    playPauseAction_ = playbackMenu->addAction(
        style()->standardIcon(QStyle::SP_MediaPlay), tr("Play / Pause"));
    playPauseAction_->setShortcut(Qt::Key_Space);
    hardwareDecodeAction_ = playbackMenu->addAction(tr("Hardware Decode"));
    hardwareDecodeAction_->setCheckable(true);
    hardwareDecodeAction_->setChecked(true);
    auto* timelinePreviewAction = playbackMenu->addAction(tr("Return to Timeline"));
    timelinePreviewAction->setShortcut(QKeySequence("Escape"));

    auto* timelineMenu = menuBar()->addMenu(tr("Timeline"));
    auto* zoomOutAction = timelineMenu->addAction(tr("Zoom Out"));
    zoomOutAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Minus));
    auto* zoomInAction = timelineMenu->addAction(tr("Zoom In"));
    zoomInAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Plus));
    auto* fitAction = timelineMenu->addAction(tr("Fit Timeline"));
    fitAction->setShortcut(QKeySequence(Qt::Key_F));
    auto* snapAction = timelineMenu->addAction(tr("Snap"));
    snapAction->setCheckable(true);
    snapAction->setChecked(true);
    snapAction->setToolTip(tr("Snap clip edges to clips, playhead and timeline start. Hold Shift to bypass."));

    auto* toolbar = addToolBar(tr("Editor"));
    toolbar->setMovable(false);
    toolbar->setFloatable(false);
    toolbar->setIconSize(QSize(18, 18));
    toolbar->addAction(openProjectAction);
    toolbar->addAction(openVideoAction);
    toolbar->addAction(saveAction);
    toolbar->addAction(splitAction);
    toolbar->addSeparator();
    toolbar->addAction(playPauseAction_);
    auto* returnAction = toolbar->addAction(tr("Timeline Preview"));
    returnAction->setToolTip(tr("Return from source preview to the timeline (Esc)"));

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
    toolbar->addAction(snapAction);

    auto* splitter = new QSplitter(Qt::Vertical, this);
    splitter->setObjectName("editorSplit");
    splitter->setChildrenCollapsible(false);
    splitter->setHandleWidth(1);
    auto* workspace = new QSplitter(Qt::Horizontal, splitter);
    workspace->setObjectName("workspace");
    workspace->setChildrenCollapsible(false);
    workspace->setHandleWidth(1);
    mediaPool_ = new MediaPoolWidget(session_, workspace);
    auto* previewPanel = new QWidget(workspace);
    previewPanel->setObjectName("previewPanel");
    auto* previewLayout = new QVBoxLayout(previewPanel);
    previewLayout->setContentsMargins(0, 0, 0, 0);
    previewLayout->setSpacing(0);
    previewLabel_ = new QLabel(tr("Timeline Preview"), previewPanel);
    previewLabel_->setMargin(8);
    previewLabel_->setStyleSheet("background: transparent; font-size: 11px;");
    previewLayout->addWidget(previewLabel_);
    videoWidget_ = new VideoWidget(previewPanel);
    videoWidget_->setMinimumSize(320, 180);
    previewLayout->addWidget(videoWidget_, 1);
    mediaInfo_ = new MediaInfoPanel(workspace);
    workspace->addWidget(new RoundedPanel(mediaPool_, workspace));
    workspace->addWidget(new RoundedPanel(previewPanel, workspace));
    workspace->addWidget(new RoundedPanel(mediaInfo_, workspace));
    workspace->setSizes({330, 610, 280});
    workspace->setStretchFactor(0, 0);
    workspace->setStretchFactor(1, 1);
    workspace->setStretchFactor(2, 0);
    timelineWidget_ = new TimelineWidget(splitter);
    timelineWidget_->setSession(session_);
    splitter->addWidget(workspace);
    splitter->addWidget(new RoundedPanel(timelineWidget_, splitter));
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 0);
    splitter->setSizes({520, 200});
    setCentralWidget(splitter);
    statusBar()->setSizeGripEnabled(false);

    connect(openProjectAction, &QAction::triggered, this, [this]() {
        const QString path = QFileDialog::getExistingDirectory(
            this, tr("Open Palmier Project Package"), QString(),
            QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
        if (!path.isEmpty()) loadProjectPackage(path);
    });
    connect(openVideoAction, &QAction::triggered, this, [this]() {
        const QString path = QFileDialog::getOpenFileName(
            this, tr("Open Video"), QString(),
            tr("Video Files (*.mp4 *.mkv *.avi *.mov *.webm);;All Files (*)"));
        if (!path.isEmpty()) {
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
    connect(importAction, &QAction::triggered, mediaPool_, &MediaPoolWidget::chooseFiles);
    connect(saveAction, &QAction::triggered, this, [this]() { saveProjectPackage(); });
    connect(saveAsAction, &QAction::triggered, this, [this]() { saveProjectPackage(true); });
    connect(undoAction, &QAction::triggered, this, [this]() {
        timelineWidget_->cancelInteraction();
        session_->undo();
    });
    connect(redoAction, &QAction::triggered, this, [this]() {
        timelineWidget_->cancelInteraction();
        session_->redo();
    });
    connect(snapAction, &QAction::toggled, timelineWidget_, &TimelineWidget::setSnappingEnabled);
    connect(splitAction, &QAction::triggered, timelineWidget_, &TimelineWidget::splitSelectedClip);
    connect(timelinePreviewAction, &QAction::triggered, this, [this]() {
        if (!timelineWidget_->cancelInteraction()) emit timelinePreviewRequested();
    });
    connect(returnAction, &QAction::triggered, this, &MainWindow::timelinePreviewRequested);
}

void MainWindow::setupConnections() {
    connect(playPauseAction_, &QAction::triggered, this, &MainWindow::playPauseClicked);
    connect(hardwareDecodeAction_, &QAction::toggled, this, &MainWindow::hardwareDecodeToggled);
    connect(timelineWidget_, &TimelineWidget::scrubStarted, this, &MainWindow::scrubStarted);
    connect(timelineWidget_, &TimelineWidget::scrubFrameRequested,
            this, &MainWindow::scrubFrameRequested);
    connect(timelineWidget_, &TimelineWidget::scrubFinished, this, &MainWindow::scrubFinished);
    connect(mediaPool_, &MediaPoolWidget::previewRequested, this, &MainWindow::previewMediaRequested);
    connect(mediaPool_, &MediaPoolWidget::placeRequested, this, [this](const QString& ref) {
        const auto result = session_->placeMedia(ref, timelineWidget_->currentFrame());
        if (!result.changed()) showError(QString::fromUtf8(result.message.c_str()));
    });
    connect(mediaPool_, &MediaPoolWidget::importFailed, this, &MainWindow::showError);
    connect(timelineWidget_, &TimelineWidget::editFailed, this, &MainWindow::showError);
    connect(session_, &heisenberg::ui::EditorSession::dirtyChanged, this, &MainWindow::updateProjectTitle);
    connect(session_, &heisenberg::ui::EditorSession::projectChanged, this, &MainWindow::updateProjectTitle);
}

bool MainWindow::hardwareDecodeEnabled() const {
    return hardwareDecodeAction_ && hardwareDecodeAction_->isChecked();
}

bool MainWindow::loadProjectPackage(const QString& path, bool showError) {
    QString error;
    if (showError && !confirmDiscardChanges()) return false;
    if (!session_->loadPackage(path, &error)) {
        if (showError) QMessageBox::critical(this, tr("Cannot Open Project"), error);
        return false;
    }
    timelineTitleLabel_->setText(timelineWidget_->timelineName());
    updateProjectTitle();
    statusBar()->showMessage(tr("Loaded %1").arg(QDir::toNativeSeparators(path)), 3500);
    return true;
}

void MainWindow::setDuration(double seconds) {
    duration_ = std::max(0.0, seconds);
}

void MainWindow::setFrameCount(qint64 frameCount) {
    frameCount_ = std::max<qint64>(0, frameCount);
    if (!sourcePreview_) timelineWidget_->setFrameCount(frameCount_);
}

void MainWindow::setCurrentTime(double seconds) {
    if (!sourcePreview_ && duration_ > 0.0 && frameCount_ > 0) {
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

void MainWindow::updateProjectTitle() {
    const auto& root = session_->editor().projectDir();
    const QString name = root.empty() ? tr("Untitled")
        : QFileInfo(QString::fromUtf8(root.c_str())).completeBaseName();
    setWindowTitle(name + "[*] - Heisenberg");
    setWindowModified(session_->isDirty());
    timelineTitleLabel_->setText(timelineWidget_->timelineName());
}

bool MainWindow::saveProjectPackage(bool saveAs) {
    QString path = QString::fromUtf8(session_->editor().projectDir().c_str());
    if (saveAs || path.isEmpty()) {
        path = QFileDialog::getSaveFileName(this, tr("Save Palmier Project"),
            path.isEmpty() ? "Untitled.palmier" : QFileInfo(path).path() + "/Copy.palmier",
            tr("Palmier Project (*.palmier)"));
        if (path.isEmpty()) return false;
        if (!path.endsWith(".palmier", Qt::CaseInsensitive)) path += ".palmier";
    }
    QString error;
    if (!session_->savePackage(path, &error)) {
        QMessageBox::critical(this, tr("Cannot Save Project"), error);
        return false;
    }
    updateProjectTitle();
    statusBar()->showMessage(tr("Project saved: %1").arg(QDir::toNativeSeparators(path)), 4000);
    return true;
}

bool MainWindow::confirmDiscardChanges() {
    if (!session_->isDirty()) return true;
    const auto answer = QMessageBox::question(this, tr("Unsaved Project"),
        tr("Save your project changes?"),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (answer == QMessageBox::Cancel) return false;
    if (answer == QMessageBox::Save) return saveProjectPackage();
    return true;
}

void MainWindow::setSourcePreview(bool source, const QString& name) {
    sourcePreview_ = source;
    previewLabel_->setText(source ? tr("Source Preview  /  %1").arg(QFileInfo(name).fileName())
                                  : tr("Timeline Preview  /  %1").arg(name));
    if (!source) showFileInfo(QString());
}

void MainWindow::showFileInfo(const QString& path) {
    const quint64 serial = ++probeSerial_;
    if (path.isEmpty()) {
        mediaInfo_->showTimelineHint();
        return;
    }
    mediaInfo_->showLoading(path);
    auto* watcher = new QFutureWatcher<heisenberg::ui::MediaInfoReport>(this);
    connect(watcher, &QFutureWatcher<heisenberg::ui::MediaInfoReport>::finished, this,
            [this, watcher, serial]() {
        const auto report = watcher->result();
        watcher->deleteLater();
        if (serial != probeSerial_) return;
        mediaInfo_->showProbe(report);
    });
    watcher->setFuture(QtConcurrent::run([path]() {
        const auto probe = heisenberg::mediainfo::probeFile(path.toUtf8().toStdString());
        return heisenberg::ui::presentProbe(probe, path);
    }));
}

void MainWindow::showError(const QString& message) {
    statusBar()->showMessage(message, 10000);
    statusBar()->setToolTip(message);
}
