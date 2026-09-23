#pragma once

#include <IPreviewer.hpp>

#include <QObject>
#include <QString>
#include <atomic>
#include <cstdint>
#include <memory>

class VideoWidget;

namespace heisenberg {
namespace ui {

class PlayerController : public QObject, public heisenberg::IPreviewer::Listener {
    Q_OBJECT

    Q_PROPERTY(bool    isPlaying   READ isPlaying   NOTIFY isPlayingChanged)
    Q_PROPERTY(double  currentTime READ currentTime NOTIFY currentTimeChanged)
    Q_PROPERTY(double  duration    READ duration    NOTIFY durationChanged)
    Q_PROPERTY(qint64  frameCount  READ frameCount  NOTIFY frameCountChanged)
    Q_PROPERTY(bool    isSeekable  READ isSeekable  NOTIFY isSeekableChanged)
    Q_PROPERTY(QString currentFile READ currentFile NOTIFY currentFileChanged)
    Q_PROPERTY(QString filterGraphPath READ filterGraphPath
               NOTIFY filterGraphPathChanged)

public:
    explicit PlayerController(QObject* parent = nullptr);
    ~PlayerController() override;

    bool    isPlaying()   const { return isPlaying_; }
    double  currentTime() const { return currentTime_; }
    double  duration()    const { return duration_; }
    qint64  frameCount()  const { return frameCount_; }
    bool    isSeekable()  const { return isSeekable_; }
    QString currentFile() const { return currentFile_; }
    QString filterGraphPath() const { return filterGraphPath_; }

public slots:
    void play();
    void pause();
    void togglePlayPause();
    void seek(double seconds);
    void beginScrub();
    void scrubToFrame(qint64 frameIndex);
    void endScrub(qint64 frameIndex);
    void stepForward(int frames = 1);
    void stepBackward(int frames = 1);
    void goToStart();
    void goToEnd();

    bool openFile(const QString& path);
    bool openPlaylist(const QString& path);
    void openFilterGraph(const QString& path);
    void closeFile();

    void shutdown();
    void bindVideoOutput(VideoWidget* widget);
    void setHardwareDecode(bool enabled);
    bool setFilterParameter(const QString& filterId,
                            const QString& name,
                            float value);
    bool getFilterParameter(const QString& filterId,
                            const QString& name,
                            float& value) const;

signals:
    void isPlayingChanged();
    void currentTimeChanged();
    void durationChanged();
    void frameCountChanged();
    void isSeekableChanged();
    void currentFileChanged();
    void filterGraphPathChanged();
    void filterGraphLoadFailed(const QString& message);

private:
    void onStateChanged(heisenberg::IPreviewer::State state) override;
    void onPositionChanged(double seconds) override;
    void onDurationChanged(double seconds) override;
    void onEndOfStream() override;
    void onOpenFailed(const std::string& reason) override;
    void onFilterGraphChanged(const std::string& path) override;
    void onFilterGraphFailed(const std::string& message) override;

    bool    isPlaying_   = false;
    double  currentTime_ = 0.0;
    double  duration_    = 0.0;
    qint64  frameCount_  = 0;
    bool    isSeekable_  = false;
    QString currentFile_;
    QString filterGraphPath_;

    std::unique_ptr<heisenberg::IPreviewer> previewer_;
    std::shared_ptr<std::atomic<bool>> previewerAlive_;
    VideoWidget* videoOutput_ = nullptr;
    bool shutdownDone_ = false;
};

} // namespace ui
} // namespace heisenberg
