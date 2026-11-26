#include <Models/ClipAppearance.hpp>
#include <gtest/gtest.h>
#include <limits>

namespace {
using namespace heisenberg;

KeyframeTrackPayload positionTrack() {
    return {JsonValue::parse(R"({"vendor":{"keep":[3,1]},"keyframes":[
        {"frame":-10,"value":{"a":0.0,"b":0.2,"future":17},"interpolationOut":"linear","opaque":true},
        {"frame":10,"value":{"a":1.0,"b":0.8,"future":23},"interpolationOut":"hold","opaque":"key"},
        {"frame":60,"value":{"a":0.2,"b":0.4},"interpolationOut":"smooth"}]})")};
}

TEST(AppearanceAuthoringTest, TargetOnlyInspectionCachesNumericDataAndLeavesFailedOutputUntouched) {
    Clip clip;
    clip.positionTrack = positionTrack();
    clip.scaleTrack = KeyframeTrackPayload{JsonValue::object()};
    clip.crop.left = 2;
    clip.transform.rotationX = 15;
    clip.opacity = std::numeric_limits<double>::quiet_NaN();
    PreparedAppearanceProperty prepared;
    std::string error;
    ASSERT_TRUE(prepareAppearanceProperty(clip, AppearanceProperty::Position, prepared, &error)) << error;
    EXPECT_EQ(prepared.keyframes(), (std::vector<int64_t>{-10, 10, 60}));
    clip.positionTrack.reset();
    ClipAppearancePropertyState state;
    ASSERT_TRUE(prepared.sample(0, state));
    EXPECT_DOUBLE_EQ(*state.sampled.centerX, 0.5);
    EXPECT_DOUBLE_EQ(*state.sampled.centerY, 0.5);
    EXPECT_FALSE(state.exactKey);
    EXPECT_TRUE(state.animated);
    ASSERT_TRUE(prepared.sample(10, state));
    EXPECT_TRUE(state.exactKey);
    EXPECT_EQ(state.interpolation, Interpolation::Hold);
    ASSERT_TRUE(prepared.sample(30, state));
    EXPECT_DOUBLE_EQ(*state.sampled.centerX, 1);
    const auto before = state.sampled;
    EXPECT_FALSE(inspectClipAppearanceProperty(clip, AppearanceProperty::Scale, 0, state, &error));
    EXPECT_EQ(state.sampled, before);
    EXPECT_FALSE(error.empty());
}

TEST(AppearanceAuthoringTest, AnimatedUpsertSamplesSiblingsAndPreservesOpaquePayloadAndOffRangeAnchors) {
    Clip clip;
    clip.durationFrames = 30;
    clip.positionTrack = positionTrack();
    clip.opacityTrack = KeyframeTrackPayload{JsonValue::object()};
    const auto original = clip;
    ClipAppearanceEdit edit;
    edit.property = AppearanceProperty::Position;
    edit.localFrame = 0;
    edit.patch.centerX = 0.7;
    Clip updated;
    ASSERT_TRUE(applyClipAppearanceEdit(clip, edit, updated));
    const auto& keys = updated.positionTrack->value["keyframes"];
    ASSERT_EQ(keys.size(), 4);
    EXPECT_EQ(keys[0], original.positionTrack->value["keyframes"][0]);
    EXPECT_EQ(keys[2], original.positionTrack->value["keyframes"][1]);
    EXPECT_EQ(keys[3], original.positionTrack->value["keyframes"][2]);
    EXPECT_DOUBLE_EQ(keys[1]["value"]["a"].get<double>(), 0.7);
    EXPECT_DOUBLE_EQ(keys[1]["value"]["b"].get<double>(), 0.5);
    EXPECT_EQ(keys[1]["interpolationOut"], "linear");
    EXPECT_EQ(updated.opacityTrack, original.opacityTrack);
    EXPECT_EQ(updated.positionTrack->value["vendor"], original.positionTrack->value["vendor"]);
    edit.localFrame = 10;
    edit.patch.centerX = 0.9;
    ASSERT_TRUE(applyClipAppearanceEdit(clip, edit, updated));
    const auto& exact = updated.positionTrack->value["keyframes"][1];
    EXPECT_EQ(exact["value"]["future"], 23);
    EXPECT_EQ(exact["opaque"], "key");
    EXPECT_EQ(exact["interpolationOut"], "hold");
    EXPECT_DOUBLE_EQ(exact["value"]["b"].get<double>(), 0.8);
    EXPECT_DOUBLE_EQ(updated.transform.centerX, original.transform.centerX);
}

TEST(AppearanceAuthoringTest, InsertUsesPrecedingInterpolationAndBeforeFirstUsesLinear) {
    Clip clip;
    clip.durationFrames = 100;
    clip.positionTrack = positionTrack();
    ClipAppearanceEdit edit;
    edit.property = AppearanceProperty::Position;
    edit.action = AppearanceEditAction::ToggleKeyframe;
    edit.localFrame = 20;
    Clip updated;
    ASSERT_TRUE(applyClipAppearanceEdit(clip, edit, updated));
    EXPECT_EQ(updated.positionTrack->value["keyframes"][2]["interpolationOut"], "hold");
    clip.positionTrack->value["keyframes"].erase(clip.positionTrack->value["keyframes"].begin());
    edit.localFrame = 0;
    ASSERT_TRUE(applyClipAppearanceEdit(clip, edit, updated));
    EXPECT_EQ(updated.positionTrack->value["keyframes"][0]["interpolationOut"], "linear");
}

TEST(AppearanceAuthoringTest, LastKeyRemovalRetainsEmptyPayloadAndConstantFallback) {
    Clip clip;
    clip.opacity = 0.8;
    clip.opacityTrack = KeyframeTrackPayload{JsonValue::parse(R"({"future":9,"keyframes":[{"frame":0,"value":0.2}]})")};
    ClipAppearanceEdit edit;
    edit.property = AppearanceProperty::Opacity;
    edit.action = AppearanceEditAction::ToggleKeyframe;
    Clip updated;
    ASSERT_TRUE(applyClipAppearanceEdit(clip, edit, updated));
    ASSERT_TRUE(updated.opacityTrack);
    EXPECT_TRUE(updated.opacityTrack->value["keyframes"].empty());
    EXPECT_EQ(updated.opacityTrack->value["future"], 9);
    ClipAppearancePropertyState state;
    ASSERT_TRUE(inspectClipAppearanceProperty(updated, AppearanceProperty::Opacity, 0, state));
    EXPECT_DOUBLE_EQ(*state.sampled.opacity, 0.8);
    EXPECT_FALSE(state.animated);
    edit.action = AppearanceEditAction::Values;
    edit.localFrame = -20;
    edit.patch.opacity = 0.6;
    Clip constant;
    ASSERT_TRUE(applyClipAppearanceEdit(updated, edit, constant));
    EXPECT_DOUBLE_EQ(constant.opacity, 0.6);
    EXPECT_EQ(constant.opacityTrack, updated.opacityTrack);
}

TEST(AppearanceAuthoringTest, KeyWritesValidateRangeAndGroupButConstantExtrasRemainAvailable) {
    Clip clip;
    clip.durationFrames = 30;
    clip.positionTrack = positionTrack();
    ClipAppearanceEdit edit;
    edit.property = AppearanceProperty::Position;
    edit.patch.centerX = 0.1;
    Clip output = clip;
    for (const auto frame : {-1, 30, 31}) {
        edit.localFrame = frame;
        EXPECT_FALSE(applyClipAppearanceEdit(clip, edit, output));
        EXPECT_EQ(output, clip);
    }
    edit.localFrame = 0;
    edit.patch.opacity = 1; // Supplied outside-group fields are rejected even if identical.
    EXPECT_FALSE(applyClipAppearanceEdit(clip, edit, output));
    edit.patch = {};
    edit.action = AppearanceEditAction::SetInterpolation;
    edit.interpolation = Interpolation::Smooth;
    EXPECT_FALSE(applyClipAppearanceEdit(clip, edit, output));
    edit.localFrame = 10;
    ASSERT_TRUE(applyClipAppearanceEdit(clip, edit, output));
    EXPECT_EQ(output.positionTrack->value["keyframes"][1]["interpolationOut"], "smooth");
    edit = {};
    edit.localFrame = 500;
    edit.patch.flipHorizontal = true;
    edit.patch.edgeSoftness = 0.2;
    ASSERT_TRUE(applyClipAppearanceEdit(clip, edit, output));
    EXPECT_TRUE(output.transform.flipHorizontal);
    EXPECT_EQ(output.positionTrack, clip.positionTrack);
}

TEST(AppearanceAuthoringTest, ScaleAndRotationUseSharedSamplingAndOpacityRejectsMalformedTarget) {
    Clip clip;
    clip.durationFrames = 30;
    clip.scaleTrack = KeyframeTrackPayload{JsonValue::parse(R"({"keyframes":[
        {"frame":0,"value":{"a":1,"b":2,"vendor":9},"interpolationOut":"smooth"},
        {"frame":20,"value":{"a":3,"b":4}}]})")};
    clip.rotationTrack = KeyframeTrackPayload{JsonValue::parse(R"({"keyframes":[
        {"frame":0,"value":0},{"frame":20,"value":720}]})")};
    clip.opacityTrack = KeyframeTrackPayload{JsonValue::parse(R"({"keyframes":[{"frame":0.0,"value":0.5}]})")};
    ClipAppearanceEdit edit;
    edit.property = AppearanceProperty::Scale;
    edit.localFrame = 0;
    edit.patch.width = 1.5;
    Clip output;
    ASSERT_TRUE(applyClipAppearanceEdit(clip, edit, output));
    const auto& value = output.scaleTrack->value["keyframes"][0]["value"];
    EXPECT_EQ(value["vendor"], 9);
    EXPECT_TRUE(value["b"].is_number_integer());
    EXPECT_EQ(value["b"], clip.scaleTrack->value["keyframes"][0]["value"]["b"]);
    edit.localFrame = 10;
    edit.patch.width = 2.5;
    ASSERT_TRUE(applyClipAppearanceEdit(clip, edit, output));
    EXPECT_DOUBLE_EQ(output.scaleTrack->value["keyframes"][1]["value"]["b"].get<double>(), 3);
    EXPECT_EQ(output.scaleTrack->value["keyframes"][1]["interpolationOut"], "smooth");
    ClipAppearancePropertyState state;
    ASSERT_TRUE(inspectClipAppearanceProperty(clip, AppearanceProperty::Rotation, 10, state));
    EXPECT_DOUBLE_EQ(*state.sampled.rotation, 360);
    EXPECT_FALSE(inspectClipAppearanceProperty(clip, AppearanceProperty::Opacity, 0, state));
    edit.property = AppearanceProperty::Opacity;
    edit.patch = {};
    edit.patch.opacity = 0.2;
    EXPECT_FALSE(applyClipAppearanceEdit(clip, edit, output));
}

TEST(AppearanceAuthoringTest, CropPartialEditUsesSampledSiblingsAndInvalidTargetIsAtomic) {
    Clip clip;
    clip.durationFrames = 30;
    clip.cropTrack = KeyframeTrackPayload{JsonValue::parse(R"({"keyframes":[
        {"frame":0,"value":{"left":0.1,"top":0.2,"right":0.3,"bottom":0.4,"vendor":3}},
        {"frame":20,"value":{"left":0.3,"top":0.2,"right":0.1,"bottom":0.4}}]})")};
    ClipAppearanceEdit edit;
    edit.property = AppearanceProperty::Crop;
    edit.localFrame = 10;
    edit.patch.cropLeft = 0.7;
    Clip output = clip;
    ASSERT_TRUE(applyClipAppearanceEdit(clip, edit, output));
    EXPECT_DOUBLE_EQ(output.cropTrack->value["keyframes"][1]["value"]["right"].get<double>(), 0.2);
    edit.patch.cropLeft = 0.8;
    Clip unchanged = clip;
    EXPECT_FALSE(applyClipAppearanceEdit(clip, edit, unchanged));
    EXPECT_EQ(unchanged, clip);
    clip.cropTrack->value["keyframes"][1]["interpolationOut"] = "future";
    EXPECT_FALSE(applyClipAppearanceEdit(clip, edit, unchanged));
}
} // namespace
