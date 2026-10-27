//
// Created by NiceFold on 2026/7/15.
//

#include "MainWidget/MainWindow.hpp"
#include "MainWidget/TimelineWidget.hpp"
#include "MainWidget/VideoWidget.hpp"
#include "MainWidget/MediaPoolWidget.hpp"
#include "MainWidget/MediaInfoPanel.hpp"
#include "MainWidget/ColorGradePanel.hpp"
#include <QTabWidget>
#include "MainWidget/RoundedPanel.hpp"
#include "Backend/EditorSession.hpp"
#include "Backend/MediaInfoProbe.hpp"

#include <QDialog>
#include <QDialogButtonBox>
#include <QComboBox>
#include <QFormLayout>
#include <QTableWidget>
#include <QHeaderView>
#include <QAction>
#include <QActionGroup>
#include <QShortcut>
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
#include <QHBoxLayout>
#include <QSlider>
#include <QSplitter>
#include <QStatusBar>
#include <QStyle>
#include <QToolBar>
#include <QToolButton>
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
        style()->standardIcon(QStyle::SP_DirOpenIcon), tr("Open Berg Project..."));
    openProjectAction->setShortcut(QKeySequence::Open);
    auto* openVideoAction = fileMenu->addAction(
        style()->standardIcon(QStyle::SP_FileIcon), tr("Open Media..."));
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
    auto* rippleAction = editMenu->addAction(tr("Ripple Delete Selected Clip"));
    rippleAction->setObjectName("rippleDeleteClip");
    rippleAction->setToolTip(tr("Delete the selected clip and close its duration on this track (Shift+Delete in the timeline)."));

    auto* exportAction = fileMenu->addAction(tr("Export Timeline..."));
    exportAction->setObjectName("exportTimeline");
    auto* colorAction = editMenu->addAction(tr("Color Settings..."));
    colorAction->setObjectName("colorSettings");
    connect(colorAction, &QAction::triggered, this, &MainWindow::showColorSettings);
    connect(exportAction, &QAction::triggered, this, [this] {
        colorGrade_->finishEditing();
        emit exportTimelineRequested();
    });
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
    auto* placementGroup = new QActionGroup(this);
    auto* overwriteMode = timelineMenu->addAction(tr("Overwrite Media Drops"));
    overwriteMode->setObjectName("overwritePlacementMode");
    auto* insertMode = timelineMenu->addAction(tr("Insert Media Drops"));
    insertMode->setObjectName("insertPlacementMode");
    for (auto* action : {overwriteMode, insertMode}) {
        action->setCheckable(true);
        placementGroup->addAction(action);
    }
    overwriteMode->setChecked(true);
    insertMode->setToolTip(tr("New media shifts subsequent clips on its track. Existing clip dragging is unchanged."));

    auto* toolbar = addToolBar(tr("Editor"));
    toolbar->setMovable(false);
    toolbar->setFloatable(false);
    toolbar->setIconSize(QSize(18, 18));
    toolbar->addAction(openProjectAction);
    toolbar->addAction(openVideoAction);
    toolbar->addAction(saveAction);
    toolbar->addAction(splitAction);
    auto* placementMode = new QComboBox(toolbar);
    placementMode->setObjectName("mediaPlacementMode");
    placementMode->addItems({tr("Overwrite"), tr("Insert")});
    placementMode->setToolTip(tr("Placement mode for media dragged from the pool"));
    toolbar->addWidget(placementMode);
    connect(placementMode, &QComboBox::currentIndexChanged, this, [overwriteMode, insertMode](int index) {
        (index == 1 ? insertMode : overwriteMode)->setChecked(true);
    });
    connect(placementGroup, &QActionGroup::triggered, this, [placementMode, insertMode](QAction* action) {
        placementMode->setCurrentIndex(action == insertMode ? 1 : 0);
    });
    auto* toolSpacer = new QWidget(toolbar);
    toolSpacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar->addWidget(toolSpacer);
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
    previewPanel->setFocusPolicy(Qt::StrongFocus);
    auto* previewLayout = new QVBoxLayout(previewPanel);
    previewLayout->setContentsMargins(0, 0, 0, 0);
    previewLayout->setSpacing(0);
    videoWidget_ = new VideoWidget(previewPanel);
    videoWidget_->setFocusPolicy(Qt::StrongFocus);
    videoWidget_->setMinimumSize(320, 180);
    previewLayout->addWidget(videoWidget_, 1);

    sourceControls_ = new QWidget(previewPanel);
    auto* sourceLayout = new QVBoxLayout(sourceControls_);
    sourceLayout->setContentsMargins(12, 8, 12, 8);
    sourceLayout->setSpacing(4);
    sourceSlider_ = new QSlider(Qt::Horizontal, sourceControls_);
    sourceSlider_->setRange(0, 1000);
    sourceSlider_->setEnabled(false);
    sourceSlider_->hide();
    sourceLayout->addWidget(sourceSlider_);
    sourceRangeControls_ = new QWidget(sourceControls_);
    sourceRangeControls_->setObjectName("sourceRangeControls");
    auto* rangeLayout = new QVBoxLayout(sourceRangeControls_);
    rangeLayout->setContentsMargins(0, 0, 0, 0);
    rangeLayout->setSpacing(3);
    auto* markButtons = new QHBoxLayout;
    markInAction_ = new QAction(tr("Mark In (I)"), this);
    markInAction_->setObjectName("markSourceIn");
    markOutAction_ = new QAction(tr("Mark Out (O)"), this);
    markOutAction_->setObjectName("markSourceOut");
    clearRangeAction_ = new QAction(tr("Clear Range"), this);
    clearRangeAction_->setObjectName("clearSourceRange");
    for (auto* action : {markInAction_, markOutAction_, clearRangeAction_}) {
        action->setEnabled(false);
        auto* button = new QToolButton(sourceRangeControls_);
        button->setDefaultAction(action);
        markButtons->addWidget(button);
    }
    markButtons->addStretch();
    rangeLayout->addLayout(markButtons);
    sourceRangeLabel_ = new QLabel(sourceRangeControls_);
    sourceRangeLabel_->setObjectName("sourceRangeLabel");
    sourceRangeLabel_->setWordWrap(true);
    rangeLayout->addWidget(sourceRangeLabel_);
    sourceRangeControls_->hide();
    sourceLayout->addWidget(sourceRangeControls_);
    connect(markInAction_, &QAction::triggered, this, &MainWindow::markSourceInRequested);
    connect(markOutAction_, &QAction::triggered, this, &MainWindow::markSourceOutRequested);
    connect(clearRangeAction_, &QAction::triggered, this, &MainWindow::clearSourceRangeRequested);
    for (const auto& binding : {std::pair{Qt::Key_I, markInAction_}, std::pair{Qt::Key_O, markOutAction_}}) {
        auto* shortcut = new QShortcut(QKeySequence(binding.first), previewPanel);
        shortcut->setContext(Qt::WidgetWithChildrenShortcut);
        shortcut->setAutoRepeat(false);
        connect(shortcut, &QShortcut::activated, binding.second, &QAction::trigger);
    }

    auto* transport = new QHBoxLayout;
    transport->setContentsMargins(0, 0, 0, 0);
    currentTimeLabel_ = new QLabel("00:00", sourceControls_);
    durationLabel_ = new QLabel("00:00", sourceControls_);
    const QString timeStyle = "font-family: Consolas, monospace; color: rgba(255,255,255,0.82);";
    currentTimeLabel_->setStyleSheet(timeStyle);
    durationLabel_->setStyleSheet(timeStyle);
    durationLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    auto* playButton = new QToolButton(sourceControls_);
    playButton->setDefaultAction(playPauseAction_);
    playButton->setAutoRaise(true);
    playButton->setIconSize(QSize(18, 18));
    transport->addWidget(currentTimeLabel_);
    transport->addStretch(1);
    transport->addWidget(playButton);
    transport->addStretch(1);
    transport->addWidget(durationLabel_);
    sourceLayout->addLayout(transport);
    previewLayout->addWidget(sourceControls_);
    connect(sourceSlider_, &QSlider::sliderPressed, this, [this]() {
        if (isPlaying_) emit playPauseClicked();
    });
    connect(sourceSlider_, &QSlider::sliderReleased, this, [this]() {
        if (duration_ <= 0.0) return;
        emit sourceSeekRequested(duration_ * sourceSlider_->value() / 1000.0);
    });
    inspector_ = new QTabWidget(workspace);
    inspector_->setObjectName("previewInspector");
    inspector_->setDocumentMode(true);
    colorGrade_ = new ColorGradePanel(session_, inspector_);
    mediaInfo_ = new MediaInfoPanel(inspector_);
    inspector_->addTab(colorGrade_, tr("Color Grade"));
    inspector_->addTab(mediaInfo_, tr("Media Info"));
    inspector_->setStyleSheet(R"(
        QTabWidget#previewInspector::pane { border: 0; }
        QTabBar::tab { background: #25262b; color: #b8bac4; padding: 8px 12px; }
        QTabBar::tab:selected { background: #303137; color: #ffffff; }
    )");
    workspace->addWidget(new RoundedPanel(mediaPool_, workspace));
    workspace->addWidget(new RoundedPanel(previewPanel, workspace));
    workspace->addWidget(new RoundedPanel(inspector_, workspace));
    workspace->setSizes({260, 560, 400});
    workspace->setStretchFactor(0, 0);
    workspace->setStretchFactor(1, 1);
    workspace->setStretchFactor(2, 0);
    timelineWidget_ = new TimelineWidget(splitter);
    timelineWidget_->setSession(session_);
    timelineWidget_->setColorGradePanelEnabled(true);
    splitter->addWidget(workspace);
    splitter->addWidget(new RoundedPanel(timelineWidget_, splitter));
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 0);
    splitter->setSizes({520, 200});
    setCentralWidget(splitter);
    statusBar()->setSizeGripEnabled(false);

    connect(openProjectAction, &QAction::triggered, this, [this]() {
        const QString path = QFileDialog::getExistingDirectory(
            this, tr("Open Berg Project Package"), QString(),
            QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
        if (!path.isEmpty()) loadProjectPackage(path);
    });
    connect(openVideoAction, &QAction::triggered, this, [this]() {
        const QString path = QFileDialog::getOpenFileName(
            this, tr("Open Media"), QString(),
            tr("Media Files (*.mp4 *.mkv *.avi *.mov *.webm *.m4v *.mts *.m2ts *.mp3 *.wav *.aac *.m4a *.flac *.ogg);;"
               "Audio Files (*.mp3 *.wav *.aac *.m4a *.flac *.ogg);;All Files (*)"));
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
        colorGrade_->finishEditing();
        timelineWidget_->cancelInteraction();
        session_->undo();
    });
    connect(redoAction, &QAction::triggered, this, [this]() {
        colorGrade_->finishEditing();
        timelineWidget_->cancelInteraction();
        session_->redo();
    });
    connect(snapAction, &QAction::toggled, timelineWidget_, &TimelineWidget::setSnappingEnabled);
    connect(splitAction, &QAction::triggered, timelineWidget_, &TimelineWidget::splitSelectedClip);
    connect(rippleAction, &QAction::triggered, timelineWidget_, &TimelineWidget::rippleDeleteSelectedClip);
    connect(insertMode, &QAction::toggled, timelineWidget_, &TimelineWidget::setInsertMode);
    connect(timelinePreviewAction, &QAction::triggered, this, [this]() {
        if (!timelineWidget_->cancelInteraction()) emit timelinePreviewRequested();
    });
}

void MainWindow::setupConnections() {
    connect(playPauseAction_, &QAction::triggered, this, &MainWindow::playPauseClicked);
    connect(hardwareDecodeAction_, &QAction::toggled, this, &MainWindow::hardwareDecodeToggled);
    connect(timelineWidget_, &TimelineWidget::scrubStarted, this, &MainWindow::scrubStarted);
    connect(timelineWidget_, &TimelineWidget::scrubFrameRequested,
            this, &MainWindow::scrubFrameRequested);
    connect(timelineWidget_, &TimelineWidget::scrubFinished, this, &MainWindow::scrubFinished);
    connect(timelineWidget_, &TimelineWidget::timelineActivated, this, [this]() {
        if (sourcePreview_) emit timelinePreviewRequested();
    });
    connect(mediaPool_, &MediaPoolWidget::previewRequested, this, &MainWindow::previewMediaRequested);
    connect(mediaPool_, &MediaPoolWidget::placeRequested, this, [this](const QString& ref) {
        colorGrade_->finishEditing();
        timelineWidget_->cancelInteraction();
        const auto result = session_->placeMedia(ref, timelineWidget_->currentFrame());
        if (!result.changed()) showError(QString::fromUtf8(result.message.c_str()));
    });
    connect(mediaPool_, &MediaPoolWidget::insertRequested, this, [this](const QString& ref) {
        colorGrade_->finishEditing();
        timelineWidget_->cancelInteraction();
        const auto result = session_->insertMedia(ref, timelineWidget_->currentFrame());
        if (!result.changed()) showError(QString::fromUtf8(result.message.c_str()));
    });
    connect(timelineWidget_, &TimelineWidget::editRequested, colorGrade_, &ColorGradePanel::finishEditing);
    connect(session_, &heisenberg::ui::EditorSession::sourceRangeChanged, this, [this](const QString& ref) {
        if (ref == sourceMediaRef_) updateSourceRange();
    });
    connect(mediaPool_, &MediaPoolWidget::importFailed, this, &MainWindow::showError);
    connect(timelineWidget_, &TimelineWidget::selectedClipChanged, this, [this](const QString& id) {
        colorGrade_->setClip(id);
        if (!id.isEmpty()) inspector_->setCurrentWidget(colorGrade_);
    });
    connect(timelineWidget_, &TimelineWidget::colorGradeRequested, this, [this](const QString& id) {
        colorGrade_->setClip(id);
        inspector_->setCurrentWidget(colorGrade_);
        colorGrade_->setFocus(Qt::OtherFocusReason);
    });
    connect(colorGrade_, &ColorGradePanel::editFailed, this, &MainWindow::showError);
    connect(inspector_, &QTabWidget::currentChanged, this, [this] { colorGrade_->finishEditing(); });
    connect(timelineWidget_, &TimelineWidget::editFailed, this, &MainWindow::showError);
    connect(session_, &heisenberg::ui::EditorSession::dirtyChanged, this, &MainWindow::updateProjectTitle);
    connect(session_, &heisenberg::ui::EditorSession::projectChanged, this, &MainWindow::updateProjectTitle);
}

bool MainWindow::hardwareDecodeEnabled() const {
    return hardwareDecodeAction_ && hardwareDecodeAction_->isChecked();
}

bool MainWindow::loadProjectPackage(const QString& path, bool showError) {
    colorGrade_->finishEditing();
    QString error;
    if (showError && !confirmDiscardChanges()) return false;
    if (!session_->loadPackage(path, &error)) {
        if (showError) QMessageBox::critical(this, tr("Cannot Open Project"), error);
        return false;
    }
    updateProjectTitle();
    statusBar()->showMessage(tr("Loaded %1").arg(QDir::toNativeSeparators(path)), 3500);
    return true;
}

void MainWindow::setDuration(double seconds) {
    duration_ = std::max(0.0, seconds);
    if (sourceSlider_) sourceSlider_->setEnabled(sourcePreview_ && duration_ > 0.0);
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
    currentTimeLabel_->setText(format(seconds));
    durationLabel_->setText(format(duration_));
    if (sourcePreview_ && sourceSlider_ && !sourceSlider_->isSliderDown() && duration_ > 0.0) {
        const int position = std::clamp(
            static_cast<int>(std::llround(std::clamp(seconds / duration_, 0.0, 1.0) * 1000.0)),
            0, 1000);
        sourceSlider_->setValue(position);
    }
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
}

bool MainWindow::saveProjectPackage(bool saveAs) {
    colorGrade_->finishEditing();
    QString path = QString::fromUtf8(session_->editor().projectDir().c_str());
    if (saveAs || path.isEmpty()) {
        QFileDialog dialog(this, tr("Save Berg Project"),
            path.isEmpty() ? "Untitled.berg" : QFileInfo(path).path() + "/Copy.berg",
            tr("Berg Project (*.berg)"));
        dialog.setObjectName("saveProjectDialog");
        dialog.setAcceptMode(QFileDialog::AcceptSave);
        dialog.setDefaultSuffix("berg");
        if (dialog.exec() != QDialog::Accepted) return false;
        path = dialog.selectedFiles().value(0);
        if (path.isEmpty()) return false;
        if (!path.endsWith(".berg", Qt::CaseInsensitive)) path += ".berg";
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
    colorGrade_->finishEditing();
    if (!session_->isDirty()) return true;
    const auto answer = QMessageBox::question(this, tr("Unsaved Project"),
        tr("Save your project changes?"),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (answer == QMessageBox::Cancel) return false;
    if (answer == QMessageBox::Save) return saveProjectPackage();
    return true;
}

void MainWindow::setSourcePreview(bool source, const QString& name) {
    Q_UNUSED(name);
    sourcePreview_ = source;
    sourceRangeControls_->setVisible(source);
    if (!source) {
        sourceMediaRef_.clear();
        sourceMarkingAvailable_ = false;
    }
    updateSourceRange();
    if (sourceSlider_) {
        sourceSlider_->setVisible(source);
        sourceSlider_->setEnabled(source && duration_ > 0.0);
        if (!source) sourceSlider_->setValue(0);
    }
    if (!source) showFileInfo(QString());
}

void MainWindow::setSourceMarking(const QString& mediaRef, bool available) {
    sourceMediaRef_ = mediaRef;
    sourceMarkingAvailable_ = available;
    updateSourceRange();
}

void MainWindow::updateSourceRange() {
    const bool enabled = sourcePreview_ && sourceMarkingAvailable_ && !sourceMediaRef_.isEmpty();
    markInAction_->setEnabled(enabled);
    markOutAction_->setEnabled(enabled);
    const auto range = session_->sourceRange(sourceMediaRef_);
    clearRangeAction_->setEnabled(enabled && (range.inSeconds || range.outSeconds));
    const auto* media = session_->editor().manifest().find(sourceMediaRef_.toStdString());
    if (!sourcePreview_ || !media) {
        sourceRangeLabel_->setText(tr("Preview a media-pool item to mark a source range."));
        return;
    }
    const double in = range.inSeconds.value_or(0.0);
    const double out = range.outSeconds.value_or(media->duration);
    sourceRangeLabel_->setText(tr("In: %1 s   Out: %2 s   Range: %3 s")
        .arg(in, 0, 'f', 3).arg(out, 0, 'f', 3).arg(std::max(0.0, out - in), 0, 'f', 3));
    sourceRangeLabel_->setToolTip(tr("Out includes the marked frame; the displayed Out is its ending time. Marks apply to overwrite, insert and media drops."));
}

void MainWindow::showFileInfo(const QString& path) {
    const quint64 serial = ++probeSerial_;
    if (path.isEmpty()) {
        mediaInfo_->showTimelineHint();
        return;
    }
    mediaInfo_->showLoading(path);
    inspector_->setCurrentWidget(mediaInfo_);
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

void MainWindow::showColorSettings() {
    colorGrade_->finishEditing();
    auto settings = session_->editor().project().colorSettings;
    QDialog dialog(this);
    dialog.setObjectName("colorSettingsDialog");
    dialog.setWindowTitle(tr("Color Settings"));
    dialog.resize(760, 420);
    auto* form = new QFormLayout(&dialog);
    const auto outputText = [](const heisenberg::OutputSelection& selection) {
        return QString::fromUtf8(selection.display.c_str()) + " / " + QString::fromUtf8(selection.view.c_str());
    };
    auto* config = new QLabel(QString::fromUtf8(settings.configId.c_str()), &dialog);
    config->setWordWrap(true); form->addRow(tr("Configuration"), config);
    auto* mode = new QComboBox(&dialog);
    mode->setObjectName("colorMode");
    mode->addItem(tr("Ordinary SDR (preserve finished appearance)"), QStringLiteral("sdr"));
    mode->addItem(tr("ACES rendering"), QStringLiteral("aces"));
    const auto originalSettings = settings;
    const bool supported = heisenberg::supportedColorSettings(settings);
    const bool legacy = supported && settings.inputPolicyVersion == 1;
    const auto matchesOutputs = [&](const char* view) {
        return settings.previewOutput == heisenberg::OutputSelection{"sRGB - Display", view} &&
            settings.exportOutput == heisenberg::OutputSelection{"Rec.1886 Rec.709 - Display", view};
    };
    if (legacy) {
        mode->addItem(tr("Legacy ACES (policy v1; preserved)"), QStringLiteral("legacy"));
        mode->setCurrentIndex(mode->count() - 1);
    } else if (supported && matchesOutputs("Un-tone-mapped")) {
        mode->setCurrentIndex(0);
    } else if (supported && matchesOutputs("ACES 2.0 - SDR 100 nits (Rec.709)")) {
        mode->setCurrentIndex(1);
    } else {
        mode->addItem(tr("Preserved saved settings"), QStringLiteral("preserved"));
        mode->setCurrentIndex(mode->count() - 1);
    }
    mode->setEnabled(supported);
    form->addRow(tr("Color mode"), mode);
    auto* summary = new QLabel(&dialog);
    summary->setObjectName("colorModeSummary");
    summary->setWordWrap(true); form->addRow(summary);
    auto* preview = new QLabel(outputText(settings.previewOutput), &dialog);
    preview->setObjectName("previewColorOutput");
    auto* exportOutput = new QLabel(outputText(settings.exportOutput), &dialog);
    exportOutput->setObjectName("exportColorOutput");
    form->addRow(tr("Preview output"), preview);
    form->addRow(tr("Export output (SDR)"), exportOutput);
    auto* hint = new QLabel(tr("Working space: ACEScg. Ordinary SDR previews finished Rec.709 video without an extra display-gamma conversion; export remains Rec.709. ACES rendering applies the ACES view. Input overrides take priority in either mode and apply to every clip using that media. Automatic input keeps warnings for unknown metadata; select camera/log inputs explicitly."), &dialog);
    hint->setWordWrap(true); form->addRow(hint);
    const auto assets = session_->editor().assets();
    auto* table = new QTableWidget(static_cast<int>(assets.size()), 2, &dialog);
    table->setObjectName("inputColorSpaces");
    table->setHorizontalHeaderLabels({tr("Media"), tr("Input color space")});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    std::vector<QComboBox*> inputs;
    for (size_t i = 0; i < assets.size(); ++i) {
        auto* item = new QTableWidgetItem(QString::fromUtf8(assets[i].name.c_str()));
        item->setFlags(item->flags() & ~Qt::ItemIsEditable); table->setItem(static_cast<int>(i), 0, item);
        auto* combo = new QComboBox(table);
        combo->setObjectName(QString("inputSpace_%1").arg(QString::fromStdString(assets[i].id)));
        combo->addItem(tr("Automatic (policy v%1)").arg(settings.inputPolicyVersion), QString());
        for (const char* space : {"sRGB Encoded Rec.709 (sRGB)", "Rec.1886 Rec.709 - Display", "Camera Rec.709", "ACEScg", "ACES2065-1", "Raw"})
            combo->addItem(QString::fromLatin1(space), QString::fromLatin1(space));
        const auto found = settings.inputColorSpaces.find(assets[i].id);
        if (found != settings.inputColorSpaces.end()) {
            const QString value = QString::fromUtf8(found->second.c_str());
            int index = combo->findData(value);
            if (index < 0) { combo->addItem(tr("Preserved: %1").arg(value), value); index = combo->count() - 1; }
            combo->setCurrentIndex(index);
        }
        if (assets[i].type != heisenberg::ClipType::Video) combo->setEnabled(false);
        table->setCellWidget(static_cast<int>(i), 1, combo); inputs.push_back(combo);
    }
    form->addRow(table);
    const auto updateMode = [&]() {
        const auto selected = mode->currentData().toString();
        if (selected == QStringLiteral("sdr") || selected == QStringLiteral("aces")) {
            settings.inputPolicyVersion = 2;
            settings.semanticsVersion = 2;
            const char* view = selected == QStringLiteral("sdr") ? "Un-tone-mapped" : "ACES 2.0 - SDR 100 nits (Rec.709)";
            settings.previewOutput = {"sRGB - Display", view};
            settings.exportOutput = {"Rec.1886 Rec.709 - Display", view};
        } else {
            settings = originalSettings;
        }
        summary->setText(!supported ? tr("Unsupported saved color settings are preserved and cannot be executed.") :
            settings.inputPolicyVersion == 1 ? tr("Legacy ACES: saved policy v1 / semantics v1. Choose a new mode to upgrade explicitly.") :
            selected == QStringLiteral("preserved") ? tr("Saved output selections are preserved. Choose a mode to replace them explicitly.") :
            selected == QStringLiteral("aces") ? tr("ACES rendering: finished SDR input policy v2 / semantics v2, with the ACES output view.") :
            tr("Ordinary SDR: code-preserving preview for finished Rec.709 video, without extra display-gamma conversion or tone mapping."));
        preview->setText(outputText(settings.previewOutput));
        exportOutput->setText(outputText(settings.exportOutput));
        for (auto* input : inputs)
            input->setItemText(0, tr("Automatic (policy v%1)").arg(settings.inputPolicyVersion));
    };
    connect(mode, qOverload<int>(&QComboBox::currentIndexChanged), &dialog, [&](int) { updateMode(); });
    updateMode();
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    for (size_t i = 0; i < assets.size(); ++i) {
        if (!inputs[i]->isEnabled()) continue;
        const auto value = inputs[i]->currentData().toString().toUtf8().toStdString();
        if (value.empty()) settings.inputColorSpaces.erase(assets[i].id);
        else settings.inputColorSpaces[assets[i].id] = value;
    }
    const auto result = session_->setColorSettings(settings);
    if (!result.changed() && !result.message.empty()) showError(QString::fromUtf8(result.message.c_str()));
}

void MainWindow::showError(const QString& message) {
    statusBar()->showMessage(message, 10000);
    statusBar()->setToolTip(message);
}
