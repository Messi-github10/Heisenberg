#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace heisenberg {

class IPreviewer {
public:
    enum class State {
        Idle,
        Loading,
        Playing,
        Paused,
        Scrubbing,
        Ended
    };

    using Task = std::function<void()>;
    using TaskDispatcher = std::function<void(Task)>;

    class Listener {
    public:
        virtual ~Listener() = default;
        virtual void onStateChanged(State /*state*/) {}
        virtual void onPositionChanged(double /*seconds*/) {}
        virtual void onDurationChanged(double /*seconds*/) {}
        virtual void onEndOfStream() {}
        virtual void onOpenFailed(const std::string& /*reason*/) {}
        virtual void onFilterGraphChanged(const std::string& /*path*/) {}
        virtual void onFilterGraphFailed(const std::string& /*message*/) {}
    };

    static void initLoader();
    static std::unique_ptr<IPreviewer> create();

    virtual ~IPreviewer() = default;

    virtual void setTaskDispatcher(TaskDispatcher dispatcher) = 0;
    virtual void setListener(Listener* listener) = 0;

    virtual void attachWindow(void* nativeSurface, int w, int h) = 0;
    virtual void detachWindow() = 0;
    virtual void resize(int w, int h) = 0;
    virtual void open(const std::string& path) = 0;
    virtual void openPlaylist(const std::string& path) = 0;
    virtual void close() = 0;
    virtual void play() = 0;
    virtual void pause() = 0;
    virtual void togglePlayPause() = 0;
    virtual void seek(double seconds) = 0;
    virtual void beginScrub() = 0;
    virtual void scrubToFrame(int64_t frameIndex) = 0;
    virtual void endScrub(int64_t frameIndex) = 0;
    virtual void stepForward(int frames = 1) = 0;
    virtual void stepBackward(int frames = 1) = 0;
    virtual void goToStart() = 0;
    virtual void goToEnd() = 0;
    virtual void setHardwareDecode(bool enabled) = 0;
    virtual void openFilterGraph(const std::string& path) = 0;
    virtual bool setNodeParameter(uint64_t nodeId,
                                  const std::string& name,
                                  float value) = 0;
    virtual bool setFilterParameter(const std::string& filterId,
                                    const std::string& name,
                                    float value) = 0;
    virtual bool getFilterParameter(const std::string& filterId,
                                    const std::string& name,
                                    float& value) const = 0;
    virtual uint64_t findNodeByFilterId(const std::string& filterId) const = 0;
    virtual void shutdown() = 0;

    virtual State state() const = 0;
    virtual bool isPlaying() const = 0;
    virtual double currentTime() const = 0;
    virtual double duration() const = 0;
    virtual bool isSeekable() const = 0;
    virtual double fps() const = 0;
    virtual int64_t frameCount() const = 0;
    virtual const std::string& filterGraphPath() const = 0;
};

} // namespace heisenberg
