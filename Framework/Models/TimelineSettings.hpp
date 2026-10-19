#pragma once

#include <Common/AudioSpec.hpp>
#include <Common/VideoSpec.hpp>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace heisenberg {

using Fraction = Rational;

class TimelineSettings {
public:
    static constexpr int kVersion = 1;
    static constexpr int kMinDimension = 1;
    static constexpr int kMaxDimension = 16384;

    TimelineSettings();
    explicit TimelineSettings(VideoSpec video, AudioSpec audio = {});
    static TimelineSettings hd1080p24();
    static TimelineSettings fromTimelineSettings(int width, int height, int fps);
    static std::vector<TimelineSettings> presets();
    static const TimelineSettings* findPreset(std::string_view id);

    int version() const {
        return kVersion;
    }

    const std::string& id() const {
        return id_;
    }

    const std::string& description() const {
        return description_;
    }

    const VideoSpec& videoSpec() const {
        return video_;
    }

    const AudioSpec& audioSpec() const {
        return audio_;
    }

    void setVideoSpec(VideoSpec video);

    void setAudioSpec(AudioSpec audio) {
        audio_ = audio;
    }

    int width() const {
        return video_.width;
    }

    int height() const {
        return video_.height;
    }

    Rational frameRate() const {
        return video_.frameRate;
    }

    Rational sampleAspect() const {
        return video_.sampleAspect;
    }

    Rational timeBase() const {
        return video_.timeBase;
    }
    
    WorkingFormat workingFormat() const;
    void setWorkingFormat(WorkingFormat format);

    int sampleRate() const {
        return audio_.sampleRate;
    }

    int channels() const {
        return audio_.channels;
    }
    
    ColorPrimaries primaries() const;
    ColorTransfer transfer() const;
    ColorRange range() const;
    void setPrimaries(ColorPrimaries value);
    void setTransfer(ColorTransfer value);
    void setRange(ColorRange value);
    double fps() const;
    double displayAspect() const;
    double secondsFromFrame(int64_t frameIndex) const;
    int64_t frameFromSeconds(double seconds) const;
    int64_t audioSamplePosition(int64_t frameIndex) const;
    int64_t audioSamplesForFrame(int64_t frameIndex) const;
    bool valid(std::string* error = nullptr) const;
    bool operator==(const TimelineSettings& other) const;

private:
    static TimelineSettings makePreset(const char* id, const char* description,
                                       int width, int height, int num, int den);
    VideoSpec video_;
    AudioSpec audio_;
    std::string id_ = "hd_1080p_24";
    std::string description_ = "HD 1080p 24 fps";
};

const char* toString(WorkingFormat format);
const char* toString(ColorPrimaries primaries);
const char* toString(ColorTransfer transfer);
const char* toString(ColorRange range);

} // namespace heisenberg
