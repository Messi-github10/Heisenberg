#include <Models/ClipAppearance.hpp>
#include <Models/KeyframeSampling.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>

namespace {
using namespace heisenberg;
using Track = std::optional<KeyframeTrackPayload> Clip::*;
constexpr std::array<Track, 5> tracks{&Clip::opacityTrack, &Clip::positionTrack,
    &Clip::scaleTrack, &Clip::rotationTrack, &Clip::cropTrack};
constexpr std::array<const char*, 5> names{"opacityTrack", "positionTrack", "scaleTrack",
    "rotationTrack", "cropTrack"};

JsonValue pair(double a, double b) { return JsonValue{{"a", a}, {"b", b}}; }
JsonValue crop(double l, double t, double r, double b) {
    return JsonValue{{"left", l}, {"top", t}, {"right", r}, {"bottom", b}};
}
KeyframeTrackPayload track(JsonValue a, JsonValue b, const std::string& interpolation = "linear",
                           int64_t left = -4, int64_t right = 4) {
    return {JsonValue{{"keyframes", JsonValue::array({
        JsonValue{{"frame", left}, {"value", a}, {"interpolationOut", interpolation}, {"opaque", "keep"}},
        JsonValue{{"frame", right}, {"value", b}}})}, {"vendor", JsonValue::array({3, 1, 2})}}};
}
Clip animated(const std::string& interpolation = "linear") {
    Clip clip;
    clip.id = "original";
    clip.durationFrames = 1;
    clip.startFrame = 100;
    clip.trimStartFrame = 12;
    clip.speed = 3.5;
    clip.opacity = 0.37;
    clip.transform.centerX = 0.41;
    clip.transform.centerY = 0.62;
    clip.transform.width = 2.0;
    clip.transform.height = 3.0;
    clip.transform.rotation = 17.0;
    clip.transform.rotationX = 7.0;
    clip.transform.rotationY = -9.0;
    clip.transform.flipHorizontal = true;
    clip.transform.flipVertical = true;
    clip.crop = {0.01, 0.02, 0.03, 0.04};
    clip.edgeRounding = 0.2;
    clip.edgeSoftness = 0.3;
    clip.blendMode = "multiply";
    clip.effects = JsonValue::parse(R"([{"type":"future.effect","opaque":[1,2,3]}])");
    clip.volumeTrack = KeyframeTrackPayload{JsonValue{{"unknownAudio", true}}};
    clip.opacityTrack = track(0.0, 1.0, interpolation);
    clip.positionTrack = track(pair(-2.0, 3.0), pair(6.0, -5.0), interpolation);
    clip.scaleTrack = track(pair(0.5, 2.0), pair(2.5, 4.0), interpolation);
    clip.rotationTrack = track(-360.0, 720.0, interpolation);
    clip.cropTrack = track(crop(0, 0.1, 0.2, 0.3), crop(0.4, 0.3, 0.1, 0), interpolation);
    return clip;
}
void expectSample(const SampledClipAppearance& sample, double w) {
    EXPECT_DOUBLE_EQ(sample.opacity, w);
    EXPECT_DOUBLE_EQ(sample.transform.centerX, -2.0 + 8.0 * w);
    EXPECT_DOUBLE_EQ(sample.transform.centerY, 3.0 - 8.0 * w);
    EXPECT_DOUBLE_EQ(sample.transform.width, 0.5 + 2.0 * w);
    EXPECT_DOUBLE_EQ(sample.transform.height, 2.0 + 2.0 * w);
    EXPECT_DOUBLE_EQ(sample.transform.rotation, -360.0 + 1080.0 * w);
    EXPECT_DOUBLE_EQ(sample.crop.left, 0.4 * w);
    EXPECT_NEAR(sample.crop.top, 0.1 + 0.2 * w, 1e-15);
    EXPECT_NEAR(sample.crop.right, 0.2 - 0.1 * w, 1e-15);
    EXPECT_NEAR(sample.crop.bottom, 0.3 * (1.0 - w), 1e-15);
    EXPECT_EQ(sample.transform.rotationX, 7.0);
    EXPECT_EQ(sample.transform.rotationY, -9.0);
    EXPECT_TRUE(sample.transform.flipHorizontal);
    EXPECT_TRUE(sample.transform.flipVertical);
}
void expectRejected(const Clip& clip, const std::string& trackName, const std::string& location = "") {
    std::string error;
    EXPECT_FALSE(validateClipAppearanceTracks(clip, &error));
    EXPECT_NE(error.find(trackName), std::string::npos) << error;
    if (!location.empty()) EXPECT_NE(error.find(location), std::string::npos) << error;
    Clip output;
    output.id = "sentinel";
    const Clip prior = output;
    EXPECT_FALSE(sampleClipAppearance(clip, -100, output, &error));
    EXPECT_EQ(output, prior);
    SampledClipAppearance light;
    light.opacity = 0.123;
    const auto priorLight = light;
    EXPECT_FALSE(sampleClipAppearance(clip, -100, light, &error));
    EXPECT_EQ(light, priorLight);
}

TEST(AppearanceTrackTest, FiveTracksAllInterpolationsAndClipLocalEndpointClamping) {
    for (const std::string mode : {"linear", "hold", "smooth"}) {
        SCOPED_TRACE(mode);
        const auto clip = animated(mode);
        ASSERT_TRUE(validateClipAppearanceTracks(clip));
        PreparedClipAppearance prepared;
        ASSERT_TRUE(prepareClipAppearance(clip, prepared));
        for (const int64_t frame : {int64_t{-100}, int64_t{-4}, int64_t{-2}, int64_t{0},
                                   int64_t{2}, int64_t{4}, int64_t{100}}) {
            SCOPED_TRACE(frame);
            const double t = std::clamp((frame + 4.0) / 8.0, 0.0, 1.0);
            const double w = mode == "hold" ? (frame >= 4 ? 1.0 : 0.0) :
                mode == "smooth" ? t * t * (3.0 - 2.0 * t) : t;
            SampledClipAppearance result;
            std::string error = "stale";
            ASSERT_TRUE(prepared.sample(frame, result, &error)) << error;
            EXPECT_TRUE(error.empty());
            expectSample(result, w);
            SampledClipAppearance direct;
            ASSERT_TRUE(sampleClipAppearance(clip, frame, direct));
            EXPECT_EQ(direct, result);
            Clip full;
            ASSERT_TRUE(sampleClipAppearance(clip, frame, full));
            Clip expected = clip;
            expected.opacity = result.opacity;
            expected.transform = result.transform;
            expected.crop = result.crop;
            EXPECT_EQ(full, expected); // Includes every opaque track/effect and constant.
        }
    }
}

TEST(AppearanceTrackTest, MissingAndEmptyTracksFallBackAndPreserveExactPayloads) {
    Clip clip;
    clip.opacity = 0.625;
    clip.transform = {0.2, -0.3, 0.7, 1.1, 42.0, 12.0, 24.0, true, false};
    clip.crop = {0.1, 0.2, 0.3, 0.4};
    for (const bool empty : {false, true}) {
        if (empty)
            for (const auto member : tracks)
                clip.*member = KeyframeTrackPayload{JsonValue{{"keyframes", JsonValue::array()}, {"opaque", true}}};
        for (const auto frame : {std::numeric_limits<int64_t>::min(), int64_t{0}, std::numeric_limits<int64_t>::max()}) {
            Clip result;
            ASSERT_TRUE(sampleClipAppearance(clip, frame, result));
            EXPECT_EQ(result, clip);
        }
    }
}

TEST(AppearanceTrackTest, OutgoingInterpolationBelongsToLeftKeyAndExactKeysSwitchImmediately) {
    Clip clip;
    clip.rotationTrack = KeyframeTrackPayload{JsonValue{{"keyframes", JsonValue::array({
        JsonValue{{"frame", -8}, {"value", 0}, {"interpolationOut", "hold"}},
        JsonValue{{"frame", 0}, {"value", 100}, {"interpolationOut", "smooth"}},
        JsonValue{{"frame", 8}, {"value", 200}, {"interpolationOut", "linear"}},
        JsonValue{{"frame", 16}, {"value", 300}, {"interpolationOut", "hold"}}})}}};
    for (const auto& [frame, expected] : std::vector<std::pair<int64_t, double>>{
             {-9, 0}, {-1, 0}, {0, 100}, {2, 115.625}, {8, 200}, {10, 225}, {16, 300}, {17, 300}}) {
        SampledClipAppearance result;
        ASSERT_TRUE(sampleClipAppearance(clip, frame, result));
        EXPECT_DOUBLE_EQ(result.transform.rotation, expected) << frame;
    }
}

TEST(AppearanceTrackTest, SingleKeyAndMissingInterpolationAreSupported) {
    auto clip = animated();
    for (const auto member : tracks) {
        auto& keys = (clip.*member)->value["keyframes"];
        keys.erase(keys.begin() + 1);
        keys[0].erase("interpolationOut");
    }
    for (const int64_t frame : {-100, -4, 100}) {
        SampledClipAppearance result;
        ASSERT_TRUE(sampleClipAppearance(clip, frame, result));
        expectSample(result, 0.0);
    }
    clip = animated();
    for (const auto member : tracks) (clip.*member)->value["keyframes"][0].erase("interpolationOut");
    SampledClipAppearance result;
    ASSERT_TRUE(sampleClipAppearance(clip, -2, result));
    expectSample(result, 0.25);
}

TEST(AppearanceTrackTest, MalformedTrackContainersRejectedForEveryTrack) {
    const std::vector<JsonValue> malformed{JsonValue::object(), nullptr, 1, true, "track",
        JsonValue::array(), JsonValue{{"keyframes", nullptr}}, JsonValue{{"keyframes", JsonValue::object()}}};
    for (size_t i = 0; i < tracks.size(); ++i)
        for (const auto& value : malformed) {
            SCOPED_TRACE(names[i]);
            auto clip = animated();
            clip.*tracks[i] = KeyframeTrackPayload{value};
            expectRejected(clip, names[i]);
        }
}

TEST(AppearanceTrackTest, EveryKeyIsValidatedIncludingMalformedFutureKeys) {
    using Mutate = std::function<void(JsonValue&)>;
    const std::vector<Mutate> bad{
        [](JsonValue& k) { k = nullptr; },
        [](JsonValue& k) { k.erase("frame"); }, [](JsonValue& k) { k.erase("value"); },
        [](JsonValue& k) { k["frame"] = true; }, [](JsonValue& k) { k["frame"] = "4"; },
        [](JsonValue& k) { k["frame"] = 4.0; }, [](JsonValue& k) { k["frame"] = 4.5; },
        [](JsonValue& k) { k["frame"] = std::numeric_limits<uint64_t>::max(); },
        [](JsonValue& k) { k["frame"] = -4; }, [](JsonValue& k) { k["frame"] = -5; },
        [](JsonValue& k) { k["interpolationOut"] = "future"; },
        [](JsonValue& k) { k["interpolationOut"] = "Smooth"; },
        [](JsonValue& k) { k["interpolationOut"] = 1; },
        [](JsonValue& k) { k["value"] = nullptr; },
        [](JsonValue& k) { k["value"] = true; },
        [](JsonValue& k) { k["value"] = "1"; }
    };
    for (size_t i = 0; i < tracks.size(); ++i)
        for (const auto& mutate : bad) {
            auto clip = animated();
            mutate((clip.*tracks[i])->value["keyframes"][1]);
            expectRejected(clip, names[i], "keyframes[1]");
        }
}

TEST(AppearanceTrackTest, InvalidValuesAreRejectedAcrossAllComponentsAndAllKeys) {
    const double inf = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    for (const double value : {inf, -inf, nan}) {
        for (size_t i = 0; i < tracks.size(); ++i) {
            const std::vector<std::string> fields = i == 1 || i == 2 ? std::vector<std::string>{"a", "b"} :
                i == 4 ? std::vector<std::string>{"left", "top", "right", "bottom"} : std::vector<std::string>{""};
            for (const auto& field : fields) {
                auto clip = animated();
                auto& v = (clip.*tracks[i])->value["keyframes"][1]["value"];
                if (field.empty()) v = value;
                else v[field] = value;
                expectRejected(clip, names[i], "keyframes[1]");
            }
        }
    }
    const std::vector<std::pair<Track, JsonValue>> invalid{
        {&Clip::opacityTrack, -0.1}, {&Clip::opacityTrack, 1.1},
        {&Clip::positionTrack, JsonValue::array({1, 2})},
        {&Clip::positionTrack, JsonValue{{"a", 1}}}, {&Clip::positionTrack, JsonValue{{"a", true}, {"b", 2}}},
        {&Clip::scaleTrack, pair(0, 1)}, {&Clip::scaleTrack, pair(1, -1)},
        {&Clip::cropTrack, crop(-0.1, 0, 0, 0)}, {&Clip::cropTrack, crop(0, 1.1, 0, 0)},
        {&Clip::cropTrack, crop(0.6, 0, 0.4, 0)}, {&Clip::cropTrack, crop(0, 0.5, 0, 0.5)},
        {&Clip::cropTrack, JsonValue{{"left", 0}, {"right", 0}, {"top", 0}}}
    };
    for (const auto& [member, value] : invalid) {
        auto clip = animated();
        (clip.*member)->value["keyframes"][1]["value"] = value;
        expectRejected(clip, member == &Clip::opacityTrack ? "opacityTrack" :
            member == &Clip::positionTrack ? "positionTrack" : member == &Clip::scaleTrack ? "scaleTrack" : "cropTrack",
            "keyframes[1]");
    }
}

TEST(AppearanceTrackTest, Int64ExtremesAndAdjacentLargeFramesKeepTheirDistance) {
    const auto lo = std::numeric_limits<int64_t>::min();
    const auto hi = std::numeric_limits<int64_t>::max();
    for (const std::string mode : {"linear", "hold", "smooth"}) {
        auto clip = animated(mode);
        for (const auto member : tracks) {
            auto& keys = (clip.*member)->value["keyframes"];
            keys[0]["frame"] = lo;
            keys[1]["frame"] = hi;
        }
        SampledClipAppearance result;
        ASSERT_TRUE(sampleClipAppearance(clip, 0, result));
        expectSample(result, mode == "hold" ? 0.0 : 0.5);
        ASSERT_TRUE(sampleClipAppearance(clip, lo, result));
        expectSample(result, 0.0);
        ASSERT_TRUE(sampleClipAppearance(clip, hi, result));
        expectSample(result, 1.0);
        for (const auto start : {lo, hi - 8}) {
            for (const auto member : tracks) {
                auto& keys = (clip.*member)->value["keyframes"];
                keys[0]["frame"] = start;
                keys[1]["frame"] = start + 8;
            }
            ASSERT_TRUE(sampleClipAppearance(clip, start + 2, result));
            expectSample(result, mode == "linear" ? 0.25 : mode == "smooth" ? 0.15625 : 0.0);
        }
    }
}

TEST(AppearanceTrackTest, DoubleValuesRemainDoubleAndLargeOppositeEndpointsDoNotOverflow) {
    Clip clip;
    clip.opacityTrack = track(0.123456789012345, 0.987654321098765);
    clip.positionTrack = track(pair(-1e300, 1e300), pair(1e300, -1e300));
    clip.rotationTrack = track(-1e300, 1e300);
    clip.scaleTrack = track(pair(1e-100, 1e100), pair(1e-100, 1e100));
    SampledClipAppearance result;
    ASSERT_TRUE(sampleClipAppearance(clip, -4, result));
    EXPECT_DOUBLE_EQ(result.opacity, 0.123456789012345);
    EXPECT_DOUBLE_EQ(result.transform.width, 1e-100);
    EXPECT_DOUBLE_EQ(result.transform.height, 1e100);
    ASSERT_TRUE(sampleClipAppearance(clip, 0, result));
    EXPECT_DOUBLE_EQ(result.transform.centerX, 0.0);
    EXPECT_DOUBLE_EQ(result.transform.centerY, 0.0);
    EXPECT_DOUBLE_EQ(result.transform.rotation, 0.0);
    EXPECT_NE(result.opacity, static_cast<double>(static_cast<float>(result.opacity)));
}

TEST(AppearanceTrackTest, InvalidConstantsRemainInvalidEvenWhenTracksReplaceThem) {
    const std::vector<std::function<void(Clip&)>> changes{
        [](Clip& c) { c.opacity = -0.1; }, [](Clip& c) { c.opacity = 1.1; },
        [](Clip& c) { c.opacity = std::numeric_limits<double>::quiet_NaN(); },
        [](Clip& c) { c.transform.centerX = std::numeric_limits<double>::infinity(); },
        [](Clip& c) { c.transform.width = 0; }, [](Clip& c) { c.transform.height = -1; },
        [](Clip& c) { c.transform.rotation = std::numeric_limits<double>::quiet_NaN(); },
        [](Clip& c) { c.crop.left = 0.9; c.crop.right = 0.1; },
        [](Clip& c) { c.edgeRounding = -0.1; }, [](Clip& c) { c.edgeSoftness = 1.1; }
    };
    for (const auto& mutate : changes) {
        auto clip = animated();
        mutate(clip);
        std::string error;
        EXPECT_FALSE(validateClipAppearanceTracks(clip, &error));
        EXPECT_FALSE(error.empty());
    }
}

TEST(AppearanceTrackTest, ScaleReplacesConstantWidthAndHeightWithoutMultiplying) {
    auto clip = animated();
    clip.transform.width = 0.8;
    clip.transform.height = 0.7;
    clip.scaleTrack = track(pair(0.5, 0.25), pair(0.5, 0.25));
    SampledClipAppearance result;
    ASSERT_TRUE(sampleClipAppearance(clip, 0, result));
    EXPECT_DOUBLE_EQ(result.transform.width, 0.5);
    EXPECT_DOUBLE_EQ(result.transform.height, 0.25);
    for (const auto value : {1e300, 1e-300}) {
        clip.transform.width = value;
        clip.scaleTrack = track(pair(1, 1), pair(value, value));
        ASSERT_TRUE(sampleClipAppearance(clip, 4, result));
        EXPECT_DOUBLE_EQ(result.transform.width, value);
        EXPECT_DOUBLE_EQ(result.transform.height, value);
    }
}

TEST(AppearanceTrackTest, SampleValidationRejectsCropRoundedToFullWidthTransactionally) {
    Clip clip;
    // Both endpoints leave a positive width, but rounding their midpoint closes it.
    clip.cropTrack = track(crop(0.37585828567644886, 0, 0.624141714323551, 0),
                           crop(0.09574311346274311, 0, 0.9042568865372568, 0));
    PreparedClipAppearance prepared;
    ASSERT_TRUE(prepareClipAppearance(clip, prepared));
    SampledClipAppearance output;
    output.opacity = 0.123;
    const auto previous = output;
    std::string error;
    EXPECT_FALSE(prepared.sample(0, output, &error));
    EXPECT_EQ(output, previous);
    EXPECT_NE(error.find("cropTrack.keyframes[0]"), std::string::npos) << error;
    Clip full;
    full.id = "sentinel";
    const Clip before = full;
    EXPECT_FALSE(sampleClipAppearance(clip, 0, full, &error));
    EXPECT_EQ(full, before);
}

TEST(AppearanceTrackTest, PreparationOwnsSnapshotAndFailurePreservesPreviousPreparedState) {
    auto clip = animated("smooth");
    PreparedClipAppearance prepared;
    ASSERT_TRUE(prepareClipAppearance(clip, prepared));
    clip.positionTrack->value = JsonValue::object();
    std::string error;
    EXPECT_FALSE(prepareClipAppearance(clip, prepared, &error));
    SampledClipAppearance result;
    ASSERT_TRUE(prepared.sample(-2, result));
    expectSample(result, 0.15625);
    auto alias = animated();
    Clip expected;
    ASSERT_TRUE(sampleClipAppearance(alias, 0, expected));
    ASSERT_TRUE(sampleClipAppearance(alias, 0, alias));
    EXPECT_EQ(alias, expected);
}

TEST(KeyframeSamplingTest, FrameReaderRejectsInvalidTypesWithoutChangingOutput) {
    int64_t frame = 42;
    for (const JsonValue value : std::vector<JsonValue>{nullptr, true, "1", 1.0, 1.5,
             JsonValue::array(), JsonValue::object(), std::numeric_limits<uint64_t>::max()}) {
        EXPECT_FALSE(keyframes::readFrame(value, frame));
        EXPECT_EQ(frame, 42);
    }
    ASSERT_TRUE(keyframes::readFrame(JsonValue(std::numeric_limits<int64_t>::min()), frame));
    EXPECT_EQ(frame, std::numeric_limits<int64_t>::min());
    ASSERT_TRUE(keyframes::readFrame(JsonValue(static_cast<uint64_t>(std::numeric_limits<int64_t>::max())), frame));
    EXPECT_EQ(frame, std::numeric_limits<int64_t>::max());
}
} // namespace
