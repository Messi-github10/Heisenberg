//
// Created by NiceFold on 2026/7/15.
//

#include "MainWidget/MainWindow.hpp"
#include "MainWidget/VideoWidget.hpp"

#include <QSlider>
#include <QPushButton>
#include <QLabel>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFileDialog>

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent) {
    setWindowTitle("Heisenberg");
    resize(960, 600);
    setupUi();
    setupConnections();
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
    progressBar_->setRange(0, 1000);

    timeLabel_ = new QLabel("00:00 / 00:00", controlBar);

    openFileBtn_ = new QPushButton("Open", controlBar);

    controlLayout->addWidget(playPauseBtn_);
    controlLayout->addWidget(progressBar_, 1);
    controlLayout->addWidget(timeLabel_);
    controlLayout->addWidget(openFileBtn_);

    mainLayout->addWidget(controlBar);
    setCentralWidget(central);
}

void MainWindow::setupConnections() {
    connect(playPauseBtn_, &QPushButton::clicked, this, &MainWindow::playPauseClicked);

    connect(progressBar_, &QSlider::sliderPressed, this, [this]() {
        sliderDragging_ = true;
    });
    connect(progressBar_, &QSlider::sliderReleased, this, [this]() {
        sliderDragging_ = false;
        double pos = static_cast<double>(progressBar_->value()) / 1000.0 * duration_;
        emit seekRequested(pos);
    });

    connect(openFileBtn_, &QPushButton::clicked, this, [this]() {
        QString path = QFileDialog::getOpenFileName(this, "Open Video", "",
            "Video Files (*.mp4 *.mkv *.avi *.mov *.webm);;All Files (*)");
        if (!path.isEmpty()) {
            emit openFileRequested(path);
        }
    });
}

void MainWindow::setDuration(double seconds) {
    duration_ = seconds;
}

void MainWindow::setCurrentTime(double seconds) {
    if (sliderDragging_) return;
    if (duration_ > 0) {
        progressBar_->setValue(static_cast<int>(seconds / duration_ * 1000.0));
    }

    auto fmt = [](double s) -> QString {
        int m = static_cast<int>(s) / 60;
        int sec = static_cast<int>(s) % 60;
        return QString("%1:%2").arg(m, 2, 10, QChar('0')).arg(sec, 2, 10, QChar('0'));
    };
    timeLabel_->setText(fmt(seconds) + " / " + fmt(duration_));
}

void MainWindow::setPlayingState(bool playing) {
    isPlaying_ = playing;
    playPauseBtn_->setText(playing ? "⏸" : "▶");
}
