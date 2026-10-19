#include "TimelineSettings.hpp"
#include <Common/MediaMath.hpp>
#include <cmath>
#include <limits>

extern "C" {
#include <libavutil/pixfmt.h>
}

namespace heisenberg {
namespace {
int64_t boundedRound(long double value) {
    if (!std::isfinite(value)) return 0;
    if (value >= static_cast<long double>(std::numeric_limits<int64_t>::max())) return std::numeric_limits<int64_t>::max();
    if (value <= static_cast<long double>(std::numeric_limits<int64_t>::min())) return std::numeric_limits<int64_t>::min();
    return static_cast<int64_t>(std::round(value));
}
}

const char* toString(WorkingFormat value) {
    switch (value) {
        case WorkingFormat::Rgba8: return "rgba8";
        case WorkingFormat::Rgba32f: return "rgba32f";
        default: return "rgba16f";
    }
}
const char* toString(ColorPrimaries value) {
    switch (value) {
        case ColorPrimaries::Bt2020: return "bt2020";
        case ColorPrimaries::DisplayP3: return "display-p3";
        default: return "bt709";
    }
}
const char* toString(ColorTransfer value) {
    switch (value) {
        case ColorTransfer::Srgb: return "srgb";
        case ColorTransfer::Pq: return "pq";
        case ColorTransfer::Hlg: return "hlg";
        case ColorTransfer::Linear: return "linear";
        default: return "bt1886";
    }
}
const char* toString(ColorRange value) {
    return value == ColorRange::Limited ? "limited" : "full";
}

TimelineSettings::TimelineSettings() {
    video_.width = 1920;
    video_.height = 1080;
    video_.frameRate = Rational(24, 1);
    video_.sampleAspect = Rational(1, 1);
    video_.timeBase = Rational(1, 24);
    setWorkingFormat(WorkingFormat::Rgba16f);
    setPrimaries(ColorPrimaries::Bt709);
    setTransfer(ColorTransfer::Bt1886);
    setRange(ColorRange::Full);
    audio_.sampleRate = 48000;
    audio_.channels = 2;
    audio_.layout = ChannelLayout::Stereo;
}

TimelineSettings::TimelineSettings(VideoSpec video, AudioSpec audio) : TimelineSettings() {
    setVideoSpec(std::move(video));
    audio_ = audio;
    id_.clear();
    description_ = "Custom timeline";
}

void TimelineSettings::setVideoSpec(VideoSpec video) {
    video_ = std::move(video);
    video_.timeBase = video_.frameRate.flipped();
}

TimelineSettings TimelineSettings::hd1080p24() { return {}; }
TimelineSettings TimelineSettings::fromTimelineSettings(int width, int height, int fps) {
    TimelineSettings settings;
    settings.id_.clear();
    settings.description_ = "Palmier timeline";
    settings.video_.width = width;
    settings.video_.height = height;
    settings.video_.frameRate = Rational(fps, 1);
    settings.video_.timeBase = Rational(1, fps);
    return settings;
}

TimelineSettings TimelineSettings::makePreset(const char* id, const char* description,
                                              int width, int height, int num, int den) {
    auto settings = fromTimelineSettings(width, height, 24);
    settings.id_ = id;
    settings.description_ = description;
    settings.video_.frameRate = Rational(num, den);
    settings.video_.timeBase = Rational(den, num);
    return settings;
}

std::vector<TimelineSettings> TimelineSettings::presets() {
    return {
        makePreset("hd_1080p_24", "HD 1080p 24 fps", 1920, 1080, 24, 1),
        makePreset("hd_1080p_2398", "HD 1080p 23.98 fps", 1920, 1080, 24000, 1001),
        makePreset("hd_1080p_25", "HD 1080p 25 fps", 1920, 1080, 25, 1),
        makePreset("hd_1080p_2997", "HD 1080p 29.97 fps", 1920, 1080, 30000, 1001),
        makePreset("hd_1080p_30", "HD 1080p 30 fps", 1920, 1080, 30, 1),
        makePreset("hd_1080p_50", "HD 1080p 50 fps", 1920, 1080, 50, 1),
        makePreset("hd_1080p_60", "HD 1080p 60 fps", 1920, 1080, 60, 1),
        makePreset("uhd_2160p_24", "UHD 2160p 24 fps", 3840, 2160, 24, 1),
        makePreset("uhd_2160p_25", "UHD 2160p 25 fps", 3840, 2160, 25, 1),
        makePreset("uhd_2160p_30", "UHD 2160p 30 fps", 3840, 2160, 30, 1),
        makePreset("uhd_2160p_50", "UHD 2160p 50 fps", 3840, 2160, 50, 1),
        makePreset("uhd_2160p_60", "UHD 2160p 60 fps", 3840, 2160, 60, 1),
    };
}

const TimelineSettings* TimelineSettings::findPreset(std::string_view id) {
    static const auto values = presets();
    for (const auto& value : values) if (value.id() == id) return &value;
    return nullptr;
}

WorkingFormat TimelineSettings::workingFormat() const {
    if (video_.format == AV_PIX_FMT_RGBA) return WorkingFormat::Rgba8;
    if (video_.format == AV_PIX_FMT_RGBAF32) return WorkingFormat::Rgba32f;
    return WorkingFormat::Rgba16f;
}

void TimelineSettings::setWorkingFormat(WorkingFormat value) {
    video_.format = value == WorkingFormat::Rgba8 ? AV_PIX_FMT_RGBA :
                    value == WorkingFormat::Rgba32f ? AV_PIX_FMT_RGBAF32 : AV_PIX_FMT_RGBAF16;
}

ColorPrimaries TimelineSettings::primaries() const {
    if (video_.colorPrimaries == AVCOL_PRI_BT2020) return ColorPrimaries::Bt2020;
    if (video_.colorPrimaries == AVCOL_PRI_SMPTE432) return ColorPrimaries::DisplayP3;
    return ColorPrimaries::Bt709;
}

void TimelineSettings::setPrimaries(ColorPrimaries value) {
    video_.colorPrimaries = value == ColorPrimaries::Bt2020 ? AVCOL_PRI_BT2020 :
                            value == ColorPrimaries::DisplayP3 ? AVCOL_PRI_SMPTE432 : AVCOL_PRI_BT709;
    video_.colorSpace = value == ColorPrimaries::Bt2020 ? AVCOL_SPC_BT2020_NCL : AVCOL_SPC_BT709;
}

ColorTransfer TimelineSettings::transfer() const {
    switch (video_.colorTransfer) {
        case AVCOL_TRC_IEC61966_2_1: return ColorTransfer::Srgb;
        case AVCOL_TRC_SMPTE2084: return ColorTransfer::Pq;
        case AVCOL_TRC_ARIB_STD_B67: return ColorTransfer::Hlg;
        case AVCOL_TRC_LINEAR: return ColorTransfer::Linear;
        default: return ColorTransfer::Bt1886;
    }
}

void TimelineSettings::setTransfer(ColorTransfer value) {
    switch (value) {
        case ColorTransfer::Srgb: video_.colorTransfer = AVCOL_TRC_IEC61966_2_1; break;
        case ColorTransfer::Pq: video_.colorTransfer = AVCOL_TRC_SMPTE2084; break;
        case ColorTransfer::Hlg: video_.colorTransfer = AVCOL_TRC_ARIB_STD_B67; break;
        case ColorTransfer::Linear: video_.colorTransfer = AVCOL_TRC_LINEAR; break;
        default: video_.colorTransfer = AVCOL_TRC_BT709; break;
    }
}

ColorRange TimelineSettings::range() const {
    return video_.colorRange == AVCOL_RANGE_MPEG ? ColorRange::Limited : ColorRange::Full;
}

void TimelineSettings::setRange(ColorRange value) {
    video_.colorRange = value == ColorRange::Limited ? AVCOL_RANGE_MPEG : AVCOL_RANGE_JPEG;
}

double TimelineSettings::fps() const {
    return frameRate().toDouble();
}

double TimelineSettings::displayAspect() const {
    return static_cast<double>(width()) / height() * sampleAspect().toDouble();
}

double TimelineSettings::secondsFromFrame(int64_t frame) const {
    return static_cast<double>(frame) * timeBase().toDouble();
}

int64_t TimelineSettings::frameFromSeconds(double seconds) const {
    return boundedRound(static_cast<long double>(seconds) * fps());
}

int64_t TimelineSettings::audioSamplePosition(int64_t frame) const {
    return samplesToPosition(frameRate(), sampleRate(), frame);
}

int64_t TimelineSettings::audioSamplesForFrame(int64_t frame) const {
    return samplesForFrame(frameRate(), sampleRate(), frame);
}

bool TimelineSettings::valid(std::string* error) const {
    if (width() < kMinDimension || width() > kMaxDimension || height() < kMinDimension || height() > kMaxDimension ||
        frameRate().numerator() <= 0 || frameRate().denominator() <= 0 ||
        sampleAspect().numerator() <= 0 || sampleAspect().denominator() <= 0 || !audio_.valid() ||
        (video_.format != AV_PIX_FMT_RGBA && video_.format != AV_PIX_FMT_RGBAF16 && video_.format != AV_PIX_FMT_RGBAF32)) {
        if (error) *error = "timeline runtime video/audio settings are invalid";
        return false;
    }
    return true;
}

bool TimelineSettings::operator==(const TimelineSettings& other) const {
    const auto& a = video_;
    const auto& b = other.video_;
    return a.width == b.width && a.height == b.height && a.frameRate == b.frameRate &&
        a.sampleAspect == b.sampleAspect && a.timeBase == b.timeBase && a.format == b.format &&
        a.colorPrimaries == b.colorPrimaries && a.colorTransfer == b.colorTransfer &&
        a.colorSpace == b.colorSpace && a.colorRange == b.colorRange && a.chromaLocation == b.chromaLocation &&
        audio_ == other.audio_ && audio_.layout == other.audio_.layout;
}

} // namespace heisenberg
