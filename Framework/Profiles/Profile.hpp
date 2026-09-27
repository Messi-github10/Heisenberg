#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace heisenberg {

struct Fraction {
    int num = 1;
    int den = 1;

    bool operator==(const Fraction&) const = default;
};

enum class WorkingFormat {
    Rgba8,
    Rgba16f,
    Rgba32f,
};

enum class ColorPrimaries {
    Bt709,
    Bt2020,
    DisplayP3,
};

enum class ColorTransfer {
    Bt1886,
    Srgb,
    Pq,
    Hlg,
    Linear,
};

enum class ColorRange {
    Full,
    Limited,
};

class Profile {
public:
    static constexpr int kVersion = 1;
    static constexpr int kMinDimension = 1;
    static constexpr int kMaxDimension = 16384;

    Profile() = default;

    static Profile hd1080p24();
    static std::vector<Profile> presets();
    static const Profile* findPreset(std::string_view id);

    bool loadFromJsonFile(const std::string& path, std::string* error = nullptr);
    bool loadFromJson(const std::string& text, std::string* error = nullptr);
    bool saveToJsonFile(const std::string& path, std::string* error = nullptr) const;
    std::string toJson() const;

    int version() const { return version_; }
    const std::string& id() const { return id_; }
    const std::string& description() const { return description_; }

    int width() const { return width_; }
    int height() const { return height_; }
    Fraction frameRate() const { return frameRate_; }
    Fraction sampleAspect() const { return sampleAspect_; }
    WorkingFormat workingFormat() const { return workingFormat_; }

    int sampleRate() const { return sampleRate_; }
    int channels() const { return channels_; }

    ColorPrimaries primaries() const { return primaries_; }
    ColorTransfer transfer() const { return transfer_; }
    ColorRange range() const { return range_; }

    double fps() const;
    Fraction timeBase() const;
    double displayAspect() const;
    double secondsFromFrame(int64_t frameIndex) const;
    int64_t frameFromSeconds(double seconds) const;
    int64_t audioSamplePosition(int64_t frameIndex) const;
    int64_t audioSamplesForFrame(int64_t frameIndex) const;

    bool operator==(const Profile&) const = default;

private:
    static Profile makePreset(const char* id,
                              const char* description,
                              int width,
                              int height,
                              int frameRateNum,
                              int frameRateDen);

    int version_ = kVersion;
    std::string id_ = "hd_1080p_24";
    std::string description_ = "HD 1080p 24 fps";
    int width_ = 1920;
    int height_ = 1080;
    Fraction frameRate_{24, 1};
    Fraction sampleAspect_{1, 1};
    WorkingFormat workingFormat_ = WorkingFormat::Rgba16f;
    int sampleRate_ = 48000;
    int channels_ = 2;
    ColorPrimaries primaries_ = ColorPrimaries::Bt709;
    ColorTransfer transfer_ = ColorTransfer::Bt1886;
    ColorRange range_ = ColorRange::Full;
};

const char* toString(WorkingFormat format);
const char* toString(ColorPrimaries primaries);
const char* toString(ColorTransfer transfer);
const char* toString(ColorRange range);

} // namespace heisenberg
