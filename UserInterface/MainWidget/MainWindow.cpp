//
// Created by NiceFold on 2026/7/15.
//

#include "MainWidget/MainWindow.hpp"
#include "MainWidget/VideoWidget.hpp"

#include <QSlider>
#include <QPushButton>
#include <QLabel>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFileDialog>
#include <QFileInfo>
#include <QCloseEvent>
#include <QSignalBlocker>

#include <algorithm>
#include <cmath>
#include <limits>

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent) {
    setWindowTitle("Heisenberg");
    resize(960, 600);
    setupUi();
    setupConnections();
}

void MainWindow::closeEvent(QCloseEvent* event) {
    emit aboutToClose();
    QMainWindow::closeEvent(event);
}

void MainWindow::setupUi() {
    auto* central = new QWidget(this);
    auto* mainLayout = new QVBoxLayout(central);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(4);

    // --- 视频区域 ---
    videoWidget_ = new VideoWidget(central);
    videoWidget_->setMinimumSize(320, 180);
    videoWidget_->setStyleSheet("background-color: black;");
    mainLayout->addWidget(videoWidget_, 1);

    // --- 控制栏 ---
    auto* controlBar = new QWidget(central);
    auto* controlLayout = new QHBoxLayout(controlBar);
    controlLayout->setContentsMargins(8, 4, 8, 4);
    controlLayout->setSpacing(8);

    playPauseBtn_ = new QPushButton("▶", controlBar);
    playPauseBtn_->setFixedWidth(36);

    progressBar_ = new QSlider(Qt::Horizontal, controlBar);
    progressBar_->setRange(0, 0);

    timeLabel_ = new QLabel("00:00 / 00:00", controlBar);

    openFileBtn_ = new QPushButton("Open", controlBar);
    openPlaylistBtn_ = new QPushButton("Open Timeline", controlBar);
    openFilterGraphBtn_ = new QPushButton("Open Graph", controlBar);
    hardwareDecodeCheck_ = new QCheckBox("HW Decode", controlBar);
    hardwareDecodeCheck_->setChecked(true);
    filterGraphLabel_ = new QLabel("Graph: none", controlBar);
    filterGraphLabel_->setMinimumWidth(150);
    filterGraphLabel_->setToolTip("No filter graph loaded");
    exposureLabel_ = new QLabel("EV --", controlBar);
    exposureSlider_ = new QSlider(Qt::Horizontal, controlBar);
    exposureSlider_->setRange(-400, 400);
    exposureSlider_->setValue(0);
    exposureSlider_->setEnabled(false);
    exposureSlider_->setFixedWidth(140);
    exposureSlider_->setToolTip("Live exposure, applied to the current frame");

    controlLayout->addWidget(playPauseBtn_);
    controlLayout->addWidget(progressBar_, 1);
    controlLayout->addWidget(timeLabel_);
    controlLayout->addWidget(openFileBtn_);
    controlLayout->addWidget(openPlaylistBtn_);
    controlLayout->addWidget(openFilterGraphBtn_);
    controlLayout->addWidget(hardwareDecodeCheck_);
    controlLayout->addWidget(filterGraphLabel_);
    controlLayout->addWidget(exposureLabel_);
    controlLayout->addWidget(exposureSlider_);

    mainLayout->addWidget(controlBar);
    setCentralWidget(central);
}

void MainWindow::setupConnections() {
    connect(playPauseBtn_, &QPushButton::clicked, this, &MainWindow::playPauseClicked);

    connect(progressBar_, &QSlider::sliderPressed, this, [this]() {
        sliderDragging_ = true;
        emit scrubStarted();
    });
    connect(progressBar_, &QSlider::sliderMoved, this, [this](int value) {
        emit scrubFrameRequested(static_cast<qint64>(value));
    });
    connect(progressBar_, &QSlider::sliderReleased, this, [this]() {
        sliderDragging_ = false;
        emit scrubFinished(static_cast<qint64>(progressBar_->value()));
    });

    connect(openFileBtn_, &QPushButton::clicked, this, [this]() {
        QString path = QFileDialog::getOpenFileName(this, "Open Video", "",
            "Video Files (*.mp4 *.mkv *.avi *.mov *.webm);;All Files (*)");
        if (!path.isEmpty()) {
            emit openFileRequested(path);
        }
    });

    connect(openPlaylistBtn_, &QPushButton::clicked, this, [this]() {
        QString path = QFileDialog::getOpenFileName(this, "Open Timeline", "",
            "Timeline JSON (*.json);;All Files (*)");
        if (!path.isEmpty()) {
            emit openPlaylistRequested(path);
        }
    });

    connect(openFilterGraphBtn_, &QPushButton::clicked, this, [this]() {
        QString path = QFileDialog::getOpenFileName(this, "Open Filter Graph", "",
            "Filter Graphs (*.json);;All Files (*)");
        if (!path.isEmpty()) {
            emit openFilterGraphRequested(path);
        }
    });

    connect(hardwareDecodeCheck_, &QCheckBox::toggled,
            this, &MainWindow::hardwareDecodeToggled);

    connect(exposureSlider_, &QSlider::valueChanged, this, [this](int value) {
        const double ev = static_cast<double>(value) / 100.0;
        exposureLabel_->setText(QString("EV %1").arg(ev, 0, 'f', 2));
        emit exposureChanged(ev);
    });
}

bool MainWindow::hardwareDecodeEnabled() const {
    return hardwareDecodeCheck_ && hardwareDecodeCheck_->isChecked();
}

void MainWindow::setDuration(double seconds) {
    duration_ = seconds;
}

void MainWindow::setFrameCount(qint64 frameCount) {
    frameCount_ = std::max<qint64>(0, frameCount);
    const qint64 lastFrame = std::max<qint64>(0, frameCount_ - 1);
    progressBar_->setRange(
        0, static_cast<int>(std::min<qint64>(
               lastFrame, std::numeric_limits<int>::max())));
}

void MainWindow::setCurrentTime(double seconds) {
    if (!sliderDragging_ && duration_ > 0.0 && frameCount_ > 0) {
        const double normalized = std::clamp(seconds / duration_, 0.0, 1.0);
        const qint64 frame = std::clamp<qint64>(
            static_cast<qint64>(
                std::llround(normalized * static_cast<double>(frameCount_))),
            0, frameCount_ - 1);
        progressBar_->setValue(static_cast<int>(std::min<qint64>(
            frame, std::numeric_limits<int>::max())));
    }

    auto fmt = [](double s) -> QString {
        int m = static_cast<int>(s) / 60;
        int sec = static_cast<int>(s) % 60;
        return QString("%1:%2").arg(m, 2, 10, QChar('0')).arg(sec, 2, 10, QChar('0'));
    };
    timeLabel_->setText(fmt(seconds) + " / " + fmt(duration_));
}

void MainWindow::setFilterGraphPath(const QString& path) {
    const QString name = QFileInfo(path).fileName();
    filterGraphLabel_->setText("Graph: " + (name.isEmpty() ? path : name));
    filterGraphLabel_->setToolTip(path);
    filterGraphLabel_->setStyleSheet(QString());
}

void MainWindow::setFilterGraphError(const QString& message) {
    filterGraphLabel_->setText("Graph: load failed");
    filterGraphLabel_->setToolTip(message);
    filterGraphLabel_->setStyleSheet("color: #c0392b;");
    setExposureEnabled(false);
}

void MainWindow::setExposure(double value) {
    if (!exposureSlider_ || !exposureLabel_) return;
    const int slider = static_cast<int>(std::lround(value * 100.0));
    const QSignalBlocker blocker(exposureSlider_);
    exposureSlider_->setValue(std::clamp(slider, exposureSlider_->minimum(),
                                         exposureSlider_->maximum()));
    exposureLabel_->setText(QString("EV %1").arg(value, 0, 'f', 2));
}

void MainWindow::setExposureEnabled(bool enabled) {
    if (exposureSlider_) exposureSlider_->setEnabled(enabled);
    if (!enabled && exposureLabel_) exposureLabel_->setText("EV --");
}

void MainWindow::setPlayingState(bool playing) {
    isPlaying_ = playing;
    playPauseBtn_->setText(playing ? "⏸" : "▶");
}
