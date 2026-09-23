#include "Profile.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <numeric>
#include <string>
#include <utility>

namespace heisenberg {
namespace {

using json = nlohmann::ordered_json;

void setError(std::string* error, const std::string& message) {
    if (error) *error = message;
}

const json& jsonField(const json& object, const char* key) {
    static const json missing;
    const auto it = object.find(key);
    return it == object.end() ? missing : *it;
}

bool expectObject(const json& value, const char* name, std::string* error) {
    if (value.is_object()) return true;
    setError(error, std::string("JSON field '") + name + "' must be an object");
    return false;
}

bool expectKeys(const json& object,
                std::initializer_list<const char*> allowed,
                const char* context,
                std::string* error) {
    for (auto it = object.begin(); it != object.end(); ++it) {
        bool found = false;
        for (const char* key : allowed) {
            if (it.key() == key) {
                found = true;
                break;
            }
        }
        if (!found) {
            setError(error, std::string(context) + " contains unknown field '" +
                                it.key() + "'");
            return false;
        }
    }
    return true;
}

bool readInt32(const json& object,
               const char* key,
               int32_t min,
               int32_t max,
               int32_t& result,
               const char* context,
               std::string* error) {
    const json& value = jsonField(object, key);
    if (value.is_null() && !object.contains(key)) {
        setError(error, std::string(context) + " is missing '" + key + "'");
        return false;
    }
    if (!value.is_number_integer()) {
        setError(error, std::string(context) + " field '" + key +
                            "' must be an integer");
        return false;
    }

    int64_t number = 0;
    if (value.is_number_unsigned()) {
        const auto unsignedNumber = value.get<uint64_t>();
        if (unsignedNumber > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            setError(error, std::string(context) + " field '" + key +
                                "' is out of range");
            return false;
        }
        number = static_cast<int64_t>(unsignedNumber);
    } else {
        number = value.get<int64_t>();
    }

    if (number < min || number > max) {
        setError(error, std::string(context) + " field '" + key +
                            "' is out of range");
        return false;
    }
    result = static_cast<int32_t>(number);
    return true;
}

bool readString(const json& object,
                const char* key,
                std::string& result,
                const char* context,
                std::string* error,
                bool required) {
    const json& value = jsonField(object, key);
    if (value.is_null() && !object.contains(key)) {
        if (!required) {
            result.clear();
            return true;
        }
        setError(error, std::string(context) + " is missing '" + key + "'");
        return false;
    }
    if (!value.is_string()) {
        setError(error, std::string(context) + " field '" + key +
                            "' must be a string");
        return false;
    }
    result = value.get<std::string>();
    return true;
}

bool reduceFraction(Fraction& value, const char* name, std::string* error) {
    if (value.num <= 0 || value.den <= 0) {
        setError(error, std::string(name) + " must be a positive rational");
        return false;
    }
    const int divisor = std::gcd(value.num, value.den);
    value.num /= divisor;
    value.den /= divisor;
    return true;
}

bool readFraction(const json& object,
                  const char* key,
                  Fraction& result,
                  std::string* error) {
    const json& value = jsonField(object, key);
    if (!expectObject(value, key, error)) return false;
    if (!expectKeys(value, {"num", "den"}, key, error)) return false;

    int32_t num = 0;
    int32_t den = 0;
    if (!readInt32(value, "num", 1, std::numeric_limits<int32_t>::max(),
                   num, key, error) ||
        !readInt32(value, "den", 1, std::numeric_limits<int32_t>::max(),
                   den, key, error)) {
        return false;
    }
    result = Fraction{num, den};
    return reduceFraction(result, key, error);
}

bool parseWorkingFormat(const std::string& text,
                        WorkingFormat& result,
                        std::string* error) {
    if (text == "rgba8") {
        result = WorkingFormat::Rgba8;
        return true;
    }
    if (text == "rgba16f") {
        result = WorkingFormat::Rgba16f;
        return true;
    }
    if (text == "rgba32f") {
        result = WorkingFormat::Rgba32f;
        return true;
    }
    setError(error, "video.workingFormat must be rgba8, rgba16f, or rgba32f");
    return false;
}

bool parsePrimaries(const std::string& text,
                    ColorPrimaries& result,
                    std::string* error) {
    if (text == "bt709") {
        result = ColorPrimaries::Bt709;
        return true;
    }
    if (text == "bt2020") {
        result = ColorPrimaries::Bt2020;
        return true;
    }
    if (text == "display-p3") {
        result = ColorPrimaries::DisplayP3;
        return true;
    }
    setError(error, "color.primaries must be bt709, bt2020, or display-p3");
    return false;
}

bool parseTransfer(const std::string& text,
                   ColorTransfer& result,
                   std::string* error) {
    if (text == "bt1886") {
        result = ColorTransfer::Bt1886;
        return true;
    }
    if (text == "srgb") {
        result = ColorTransfer::Srgb;
        return true;
    }
    if (text == "pq") {
        result = ColorTransfer::Pq;
        return true;
    }
    if (text == "hlg") {
        result = ColorTransfer::Hlg;
        return true;
    }
    if (text == "linear") {
        result = ColorTransfer::Linear;
        return true;
    }
    setError(error, "color.transfer must be bt1886, srgb, pq, hlg, or linear");
    return false;
}

bool parseRange(const std::string& text,
                ColorRange& result,
                std::string* error) {
    if (text == "full") {
        result = ColorRange::Full;
        return true;
    }
    if (text == "limited") {
        result = ColorRange::Limited;
        return true;
    }
    setError(error, "color.range must be full or limited");
    return false;
}

bool parseSampleRate(int32_t value, int& result, std::string* error) {
    if (value == 44100 || value == 48000 || value == 96000) {
        result = value;
        return true;
    }
    setError(error, "audio.sampleRate must be 44100, 48000, or 96000");
    return false;
}

bool parseChannels(int32_t value, int& result, std::string* error) {
    if (value == 1 || value == 2) {
        result = value;
        return true;
    }
    setError(error, "audio.channels must be 1 or 2");
    return false;
}

bool parseId(const std::string& value, std::string& result, std::string* error) {
    if (value.empty()) {
        result.clear();
        return true;
    }
    for (const char ch : value) {
        const bool ok = (ch >= 'a' && ch <= 'z') ||
                        (ch >= '0' && ch <= '9') ||
                        ch == '_';
        if (!ok) {
            setError(error, "id must match [a-z0-9_]+");
            return false;
        }
    }
    result = value;
    return true;
}

json fractionJson(Fraction value) {
    json result;
    result["num"] = value.num;
    result["den"] = value.den;
    return result;
}

} // namespace

const char* toString(WorkingFormat format) {
    switch (format) {
        case WorkingFormat::Rgba8: return "rgba8";
        case WorkingFormat::Rgba16f: return "rgba16f";
        case WorkingFormat::Rgba32f: return "rgba32f";
    }
    return "rgba16f";
}

const char* toString(ColorPrimaries primaries) {
    switch (primaries) {
        case ColorPrimaries::Bt709: return "bt709";
        case ColorPrimaries::Bt2020: return "bt2020";
        case ColorPrimaries::DisplayP3: return "display-p3";
    }
    return "bt709";
}

const char* toString(ColorTransfer transfer) {
    switch (transfer) {
        case ColorTransfer::Bt1886: return "bt1886";
        case ColorTransfer::Srgb: return "srgb";
        case ColorTransfer::Pq: return "pq";
        case ColorTransfer::Hlg: return "hlg";
        case ColorTransfer::Linear: return "linear";
    }
    return "bt1886";
}

const char* toString(ColorRange range) {
    switch (range) {
        case ColorRange::Full: return "full";
        case ColorRange::Limited: return "limited";
    }
    return "full";
}

Profile Profile::makePreset(const char* id,
                            const char* description,
                            int width,
                            int height,
                            int frameRateNum,
                            int frameRateDen) {
    Profile profile;
    profile.id_ = id;
    profile.description_ = description;
    profile.width_ = width;
    profile.height_ = height;
    profile.frameRate_ = {frameRateNum, frameRateDen};
    return profile;
}

Profile Profile::hd1080p24() {
    return Profile();
}

std::vector<Profile> Profile::presets() {
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

const Profile* Profile::findPreset(std::string_view id) {
    static const std::vector<Profile> presets = Profile::presets();
    for (const Profile& profile : presets) {
        if (profile.id_ == id) return &profile;
    }
    return nullptr;
}

bool Profile::loadFromJsonFile(const std::string& path, std::string* error) {
    std::ifstream file(std::filesystem::u8path(path), std::ios::binary);
    if (!file) {
        setError(error, "Failed to open Profile JSON '" + path + "'");
        return false;
    }

    std::string text((std::istreambuf_iterator<char>(file)),
                     std::istreambuf_iterator<char>());
    return loadFromJson(text, error);
}

bool Profile::loadFromJson(const std::string& text, std::string* error) {
    json root;
    try {
        root = json::parse(text);
    } catch (const json::parse_error& parseError) {
        setError(error, std::string("Invalid Profile JSON: ") + parseError.what());
        return false;
    }
    if (!root.is_object()) {
        setError(error, "Profile JSON root must be an object");
        return false;
    }
    if (!expectKeys(root,
                    {"version", "id", "description", "video", "audio", "color"},
                    "profile", error)) {
        return false;
    }

    Profile parsed;
    int32_t version = 0;
    if (!readInt32(root, "version", kVersion, kVersion, version, "profile", error)) {
        return false;
    }
    parsed.version_ = version;

    std::string id;
    std::string description;
    if (!readString(root, "id", id, "profile", error, false) ||
        !parseId(id, parsed.id_, error) ||
        !readString(root, "description", description, "profile", error, false)) {
        return false;
    }
    parsed.description_ = std::move(description);

    const json& video = jsonField(root, "video");
    const json& audio = jsonField(root, "audio");
    const json& color = jsonField(root, "color");
    if (!expectObject(video, "video", error) ||
        !expectObject(audio, "audio", error) ||
        !expectObject(color, "color", error)) {
        return false;
    }
    if (!expectKeys(video,
                    {"width", "height", "frameRate", "sampleAspect", "workingFormat"},
                    "video", error) ||
        !expectKeys(audio, {"sampleRate", "channels"}, "audio", error) ||
        !expectKeys(color, {"primaries", "transfer", "range"}, "color", error)) {
        return false;
    }

    int32_t width = 0;
    int32_t height = 0;
    std::string workingFormatText;
    if (!readInt32(video, "width", kMinDimension, kMaxDimension,
                   width, "video", error) ||
        !readInt32(video, "height", kMinDimension, kMaxDimension,
                   height, "video", error) ||
        !readFraction(video, "frameRate", parsed.frameRate_, error) ||
        !readFraction(video, "sampleAspect", parsed.sampleAspect_, error) ||
        !readString(video, "workingFormat", workingFormatText, "video", error, true) ||
        !parseWorkingFormat(workingFormatText, parsed.workingFormat_, error)) {
        return false;
    }
    parsed.width_ = width;
    parsed.height_ = height;

    int32_t sampleRateValue = 0;
    int32_t channelsValue = 0;
    if (!readInt32(audio, "sampleRate", 1, std::numeric_limits<int32_t>::max(),
                   sampleRateValue, "audio", error) ||
        !parseSampleRate(sampleRateValue, parsed.sampleRate_, error) ||
        !readInt32(audio, "channels", 1, std::numeric_limits<int32_t>::max(),
                   channelsValue, "audio", error) ||
        !parseChannels(channelsValue, parsed.channels_, error)) {
        return false;
    }

    std::string primariesText;
    std::string transferText;
    std::string rangeText;
    if (!readString(color, "primaries", primariesText, "color", error, true) ||
        !parsePrimaries(primariesText, parsed.primaries_, error) ||
        !readString(color, "transfer", transferText, "color", error, true) ||
        !parseTransfer(transferText, parsed.transfer_, error) ||
        !readString(color, "range", rangeText, "color", error, true) ||
        !parseRange(rangeText, parsed.range_, error)) {
        return false;
    }

    *this = std::move(parsed);
    return true;
}

std::string Profile::toJson() const {
    json video;
    video["width"] = width_;
    video["height"] = height_;
    video["frameRate"] = fractionJson(frameRate_);
    video["sampleAspect"] = fractionJson(sampleAspect_);
    video["workingFormat"] = toString(workingFormat_);

    json audio;
    audio["sampleRate"] = sampleRate_;
    audio["channels"] = channels_;

    json color;
    color["primaries"] = toString(primaries_);
    color["transfer"] = toString(transfer_);
    color["range"] = toString(range_);

    json root;
    root["version"] = version_;
    if (!id_.empty()) root["id"] = id_;
    if (!description_.empty()) root["description"] = description_;
    root["video"] = std::move(video);
    root["audio"] = std::move(audio);
    root["color"] = std::move(color);
    return root.dump(2);
}

bool Profile::saveToJsonFile(const std::string& path, std::string* error) const {
    const auto filePath = std::filesystem::u8path(path);
    std::error_code ec;
    if (filePath.has_parent_path()) {
        std::filesystem::create_directories(filePath.parent_path(), ec);
        if (ec) {
            setError(error, "Failed to create directory for Profile JSON '" + path + "'");
            return false;
        }
    }

    std::ofstream file(filePath, std::ios::binary | std::ios::trunc);
    if (!file) {
        setError(error, "Failed to write Profile JSON '" + path + "'");
        return false;
    }
    file << toJson();
    file << '\n';
    if (!file) {
        setError(error, "Failed to write Profile JSON '" + path + "'");
        return false;
    }
    return true;
}

double Profile::fps() const {
    return static_cast<double>(frameRate_.num) /
           static_cast<double>(frameRate_.den);
}

Fraction Profile::timeBase() const {
    return Fraction{frameRate_.den, frameRate_.num};
}

double Profile::displayAspect() const {
    return (static_cast<double>(width_) * sampleAspect_.num) /
           (static_cast<double>(height_) * sampleAspect_.den);
}

double Profile::secondsFromFrame(int64_t frameIndex) const {
    return static_cast<double>(frameIndex) * frameRate_.den /
           static_cast<double>(frameRate_.num);
}

int64_t Profile::frameFromSeconds(double seconds) const {
    if (!std::isfinite(seconds)) return 0;
    return static_cast<int64_t>(std::llround(
        seconds * static_cast<double>(frameRate_.num) /
        static_cast<double>(frameRate_.den)));
}

int64_t Profile::audioSamplePosition(int64_t frameIndex) const {
    if (frameIndex <= 0) return 0;
    return (frameIndex * static_cast<int64_t>(sampleRate_) *
            static_cast<int64_t>(frameRate_.den)) /
           static_cast<int64_t>(frameRate_.num);
}

int64_t Profile::audioSamplesForFrame(int64_t frameIndex) const {
    return audioSamplePosition(frameIndex + 1) - audioSamplePosition(frameIndex);
}

} // namespace heisenberg
