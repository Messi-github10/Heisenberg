//
// Created by NiceFold on 2026/6/30.
//

#pragma once

#include <QObject>
#include <QString>
#include <memory>

struct AVFrame;

namespace heisenberg {
namespace ctrl   { class PlaybackController; }
namespace renderer { class IPreviewer; class GpuContext; class SwapChain; }
}

class VideoWidget;

namespace heisenberg {
namespace ui {

class PlayerController : public QObject {
    Q_OBJECT

    Q_PROPERTY(bool    isPlaying   READ isPlaying   NOTIFY isPlayingChanged)
    Q_PROPERTY(double  currentTime READ currentTime NOTIFY currentTimeChanged)
    Q_PROPERTY(double  duration    READ duration    NOTIFY durationChanged)
    Q_PROPERTY(qint64  frameCount  READ frameCount  NOTIFY frameCountChanged)
    Q_PROPERTY(bool    isSeekable  READ isSeekable  NOTIFY isSeekableChanged)
    Q_PROPERTY(QString currentFile READ currentFile NOTIFY currentFileChanged)

public:
    explicit PlayerController(QObject* parent = nullptr);
    ~PlayerController() override;

    bool    isPlaying()   const { return isPlaying_; }
    double  currentTime() const { return currentTime_; }
    double  duration()    const { return duration_; }
    qint64  frameCount()  const { return frameCount_; }
    bool    isSeekable()  const { return isSeekable_; }
    QString currentFile() const { return currentFile_; }

    void setPlaybackController(heisenberg::ctrl::PlaybackController* ctrl);

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
    void closeFile();

    /// 显式释放 GPU 资源（程序退出前调用）
    void shutdown();

    /// 绑定 VideoWidget，初始化 Vulkan 渲染管线
    void bindVideoOutput(VideoWidget* widget);

signals:
    void isPlayingChanged();
    void currentTimeChanged();
    void durationChanged();
    void frameCountChanged();
    void isSeekableChanged();
    void currentFileChanged();

private slots:
    void onFrameDecoded(std::shared_ptr<AVFrame> frame);

private:
    void initPipeline(VideoWidget* widget);

    bool    isPlaying_   = false;
    double  currentTime_ = 0.0;
    double  duration_    = 0.0;
    qint64  frameCount_  = 0;
    bool    isSeekable_  = false;
    QString currentFile_;

    heisenberg::ctrl::PlaybackController* ctrl_ = nullptr;

    std::unique_ptr<heisenberg::renderer::GpuContext> gpuCtx_;
    std::unique_ptr<heisenberg::renderer::IPreviewer> previewer_;
    VideoWidget* videoOutput_ = nullptr;

    int videoWidth_  = 0;
    int videoHeight_ = 0;
    bool shutdownDone_ = false;
    std::shared_ptr<AVFrame> lastFrame_;
};

} // namespace ui
} // namespace heisenberg
