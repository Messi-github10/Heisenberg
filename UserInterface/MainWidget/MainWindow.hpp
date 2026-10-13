//
// Created by NiceFold on 2026/7/15.
//

#pragma once

#include <QMainWindow>

class VideoWidget;
class TimelineWidget;
class MediaInfoPanel;
class QAction;
class QLabel;
class QSlider;
class QCloseEvent;
class MediaPoolWidget;
namespace heisenberg::ui { class EditorSession; }

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(heisenberg::ui::EditorSession* session, QWidget* parent = nullptr);
    ~MainWindow() override = default;

    VideoWidget* videoWidget() const { return videoWidget_; }
    TimelineWidget* timelineWidget() const { return timelineWidget_; }
    bool hardwareDecodeEnabled() const;
    bool loadProjectPackage(const QString& path, bool showError = true);
    bool saveProjectPackage(bool saveAs = false);
    void setSourcePreview(bool source, const QString& name);
    void showFileInfo(const QString& path);
    void showError(const QString& message);

signals:
    void aboutToClose();
    void playPauseClicked();
    void scrubStarted();
    void scrubFrameRequested(qint64 frameIndex);
    void scrubFinished(qint64 frameIndex);
    void sourceSeekRequested(double seconds);
    void openFileRequested(const QString& path);
    void openFilterGraphRequested(const QString& path);
    void hardwareDecodeToggled(bool enabled);
    void previewMediaRequested(const QString& mediaRef);
    void timelinePreviewRequested();

public slots:
    void setDuration(double seconds);
    void setFrameCount(qint64 frameCount);
    void setCurrentTime(double seconds);
    void setPlayingState(bool playing);
    void setFilterGraphPath(const QString& path);
    void setFilterGraphError(const QString& message);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void setupUi();
    void setupConnections();
    bool confirmDiscardChanges();
    void updateProjectTitle();

    VideoWidget* videoWidget_ = nullptr;
    TimelineWidget* timelineWidget_ = nullptr;
    QAction* playPauseAction_ = nullptr;
    QAction* hardwareDecodeAction_ = nullptr;
    QLabel* currentTimeLabel_ = nullptr;
    QLabel* durationLabel_ = nullptr;
    QWidget* sourceControls_ = nullptr;
    QSlider* sourceSlider_ = nullptr;
    heisenberg::ui::EditorSession* session_;
    MediaPoolWidget* mediaPool_ = nullptr;
    MediaInfoPanel* mediaInfo_ = nullptr;
    bool sourcePreview_ = false;
    quint64 probeSerial_ = 0;

    double duration_ = 0.0;
    qint64 frameCount_ = 0;
    bool isPlaying_  = false;
};
