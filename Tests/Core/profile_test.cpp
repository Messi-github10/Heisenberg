#include "Profile.hpp"

#include <gtest/gtest.h>

#include <string>

namespace {

bool loadFails(const std::string& json) {
    heisenberg::Profile profile;
    std::string error;
    const bool ok = profile.loadFromJson(json, &error);
    return !ok && !error.empty();
}

} // namespace

TEST(ProfileTest, BuiltInPresetsRoundTrip) {
    const auto presets = heisenberg::Profile::presets();
    ASSERT_GE(presets.size(), 12u);

    for (const heisenberg::Profile& preset : presets) {
        ASSERT_FALSE(preset.id().empty());
        const heisenberg::Profile* found = heisenberg::Profile::findPreset(preset.id());
        ASSERT_NE(found, nullptr) << preset.id();
        EXPECT_EQ(*found, preset) << preset.id();

        heisenberg::Profile roundTrip;
        std::string error;
        ASSERT_TRUE(roundTrip.loadFromJson(preset.toJson(), &error)) << error;
        EXPECT_EQ(roundTrip, preset) << preset.id();
    }
}

TEST(ProfileTest, AudioClockMatchesFrameRate) {
    const heisenberg::Profile hd24 = heisenberg::Profile::hd1080p24();
    EXPECT_EQ(hd24.audioSamplesForFrame(0), 2000);

    const heisenberg::Profile* ntsc =
        heisenberg::Profile::findPreset("hd_1080p_2997");
    ASSERT_NE(ntsc, nullptr);
    const int64_t first = ntsc->audioSamplesForFrame(0);
    const int64_t second = ntsc->audioSamplesForFrame(1);
    EXPECT_EQ(first + second, 3203);
    EXPECT_TRUE(first == 1601 || first == 1602);
    EXPECT_EQ(ntsc->frameFromSeconds(ntsc->secondsFromFrame(1000)), 1000);
}

TEST(ProfileTest, RejectsInvalidJson) {
    EXPECT_TRUE(loadFails(R"({"version":2,"video":{},"audio":{},"color":{}})"));
    EXPECT_TRUE(loadFails(R"({
        "version": 1,
        "video": {
          "width": 1920, "height": 1080,
          "frameRate": 23.976,
          "sampleAspect": {"num":1,"den":1},
          "workingFormat": "rgba16f"
        },
        "audio": {"sampleRate":48000,"channels":2},
        "color": {"primaries":"bt709","transfer":"bt1886","range":"full"}
      })"));
    EXPECT_TRUE(loadFails(R"({
        "version": 1,
        "extra": true,
        "video": {
          "width": 1920, "height": 1080,
          "frameRate": {"num":24,"den":1},
          "sampleAspect": {"num":1,"den":1},
          "workingFormat": "rgba16f"
        },
        "audio": {"sampleRate":48000,"channels":2},
        "color": {"primaries":"bt709","transfer":"bt1886","range":"full"}
      })"));
    EXPECT_TRUE(loadFails(R"({
        "version": 1,
        "video": {
          "width": 1920, "height": 1080,
          "frameRate": {"num":24,"den":1},
          "sampleAspect": {"num":1,"den":1},
          "workingFormat": "rgba16f"
        },
        "audio": {"sampleRate":32000,"channels":2},
        "color": {"primaries":"bt709","transfer":"bt1886","range":"full"}
      })"));
}
