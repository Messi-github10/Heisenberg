#include <Compositing/Grade/GradeCompiler.hpp>
#include <Compositing/Grade/ParameterSampler.hpp>
#include <gtest/gtest.h>
#include <algorithm>
#include <limits>

namespace {
using namespace heisenberg;
using namespace heisenberg::grade;
using namespace heisenberg::filtergraph;

Clip withEffects(const char* effects) {
    Clip clip;
    clip.id = "clip";
    clip.startFrame = 100;
    clip.trimStartFrame = 40;
    clip.speed = 2.0;
    clip.effects = JsonValue::parse(effects);
    return clip;
}

float firstFloat(const CompiledGrade& grade, const std::string& name) {
    return std::get<float>(grade.stages.at(0).parameterUpdates.at(0).parameters.at(name));
}

} // namespace

TEST(GradeCompilerTest, NoEffectsDisabledAndDefaultsAreNeutralWithoutGraphs) {
    CompiledGrade grade;
    EXPECT_TRUE(GradeCompiler::compile(Clip{}, 0, grade));
    EXPECT_TRUE(grade.neutral());
    auto clip = withEffects(R"([{"id":"basic-grade","type":"video.basicGrade","enabled":true,"params":{"exposure":{"value":0},"contrast":{"value":1},"saturation":{"value":1}}},{"type":"video.blur","params":{}},{"type":"future.geometry","enabled":false}])");
    EXPECT_TRUE(GradeCompiler::compile(clip, 0, grade));
    EXPECT_TRUE(grade.neutral());
    EXPECT_TRUE(grade.diagnostics.empty());
}

TEST(GradeCompilerTest, PreservesStackOrderAndCoalescesOnlyAdjacentDomains) {
    auto clip = withEffects(R"([{"id":"basic-grade","type":"video.basicGrade","params":{"exposure":{"value":1},"contrast":{"value":1.5},"saturation":{"value":0.5}}},{"id":"blur","type":"video.blur","params":{"radius":{"value":2}}},{"id":"again","type":"video.exposure","params":{"exposure":{"value":-1}}}])");
    CompiledGrade grade;
    std::string error;
    ASSERT_TRUE(GradeCompiler::compile(clip, 0, grade, &error)) << error;
    ASSERT_EQ(grade.stages.size(), 3u);
    EXPECT_EQ(grade.stages[0].domain, GradeDomain::ACEScg);
    EXPECT_EQ(grade.stages[1].domain, GradeDomain::ACEScct);
    EXPECT_EQ(grade.stages[2].domain, GradeDomain::ACEScg);
    EXPECT_EQ(grade.stages[1].document.nodes()[2].filterId, "grade_contrast_acescct");
    EXPECT_EQ(grade.stages[1].document.nodes()[3].filterId, "grade_saturation_acescct");
    EXPECT_EQ(grade.stages[2].document.nodes()[2].filterId, "grade_gaussian_blur_acescg");
    EXPECT_EQ(grade.stages[2].document.nodes()[3].filterId, "grade_exposure_acescg");
    for (const auto& stage : grade.stages) {
        EXPECT_TRUE(stage.document.validate(&error)) << error;
        EXPECT_EQ(stage.document.edges().size(), stage.parameterUpdates.size() + 1);
        EXPECT_FALSE(stage.topologyKey.empty());
        for (const auto& update : stage.parameterUpdates) {
            const auto& nodes = stage.document.nodes();
            auto found = std::find_if(nodes.begin(), nodes.end(), [&](const auto& node) { return node.id == update.nodeId; });
            ASSERT_NE(found, nodes.end());
            for (const auto& [key, value] : update.parameters) EXPECT_EQ(found->parameter.at(key), value);
        }
    }
    std::swap((*clip.effects)[0], (*clip.effects)[1]);
    ASSERT_TRUE(GradeCompiler::compile(clip, 0, grade));
    EXPECT_EQ(grade.stages[0].document.nodes()[2].filterId, "grade_gaussian_blur_acescg");
}

TEST(GradeCompilerTest, SamplesClipLocalHoldLinearAndEndpointExtrapolation) {
    auto clip = withEffects(R"([{"id":"exp","type":"video.exposure","params":{"exposure":{"value":9,"track":{"keyframes":[{"frame":2,"value":1,"interpolationOut":"hold"},{"frame":6,"value":3,"interpolationOut":"linear"},{"frame":10,"value":7,"interpolationOut":"hold"}]}}}}])");
    const std::pair<int64_t, float> samples[] = {{-100,1.0f},{0,1.0f},{2,1.0f},{5,1.0f},{6,3.0f},{8,5.0f},{10,7.0f},{999,7.0f}};
    for (const auto& [frame, expected] : samples) {
        CompiledGrade grade;
        ASSERT_TRUE(GradeCompiler::compile(clip, frame, grade));
        EXPECT_FLOAT_EQ(firstFloat(grade, "exposure"), expected) << frame;
    }
}

TEST(GradeCompilerTest, AnimatedNeutralFastPathAndTopologyKeysIgnoreValues) {
    auto clip = withEffects(R"([{"type":"video.exposure","params":{"exposure":{"track":{"keyframes":[{"frame":0,"value":0,"interpolationOut":"linear"},{"frame":10,"value":2}]}}}}])");
    CompiledGrade neutral, a, b;
    ASSERT_TRUE(GradeCompiler::compile(clip, 0, neutral));
    EXPECT_TRUE(neutral.neutral());
    ASSERT_TRUE(GradeCompiler::compile(clip, 2, a));
    ASSERT_TRUE(GradeCompiler::compile(clip, 8, b));
    EXPECT_EQ(a.stages[0].topologyKey, b.stages[0].topologyKey);
    EXPECT_NE(firstFloat(a,"exposure"), firstFloat(b,"exposure"));
    EXPECT_EQ(a.stages[0].document.edges(), b.stages[0].document.edges());
}

TEST(GradeCompilerTest, SmoothFixtureIsDiagnosedAndStrictRefusesEvenBeforeItsKey) {
    auto clip = withEffects(R"([{"id":"effect-a","type":"video.blur","enabled":true,"params":{"radius":{"value":4.0,"track":{"keyframes":[{"frame":3,"value":5.0,"interpolationOut":"smooth"}]}}}}])");
    CompiledGrade grade;
    std::string error;
    EXPECT_FALSE(GradeCompiler::compile(clip, 0, grade, &error));
    EXPECT_TRUE(grade.neutral());
    ASSERT_FALSE(grade.diagnostics.empty());
    EXPECT_EQ(grade.diagnostics[0].code, "unsupported_interpolation");
    EXPECT_EQ(grade.diagnostics[0].effectId, "effect-a");
    EXPECT_EQ(grade.diagnostics[0].parameter, "radius");
    EXPECT_FALSE(error.empty());
    ASSERT_TRUE(GradeCompiler::compile(clip, 0, grade, &error, {.strict=false})) << error;
    ASSERT_EQ(grade.stages.size(),1u);
    EXPECT_EQ(std::get<int32_t>(grade.stages[0].parameterUpdates[0].parameters.at("blurRadius")), 5);
    EXPECT_FALSE(grade.diagnostics.empty());
}

TEST(GradeCompilerTest, UnknownEffectsAndParametersNeverDisappearSilently) {
    auto clip = withEffects(R"([{"id":"geometry","type":"video.3d","params":{}},{"id":"exp","type":"video.exposure","params":{"exposure":{"value":1},"mystery":{"value":2}}}])");
    CompiledGrade grade;
    EXPECT_FALSE(GradeCompiler::compile(clip, 0, grade));
    EXPECT_TRUE(grade.neutral());
    ASSERT_TRUE(GradeCompiler::compile(clip, 0, grade, nullptr, {.strict=false}));
    EXPECT_FALSE(grade.neutral());
    ASSERT_EQ(grade.diagnostics.size(),2u);
    EXPECT_EQ(grade.diagnostics[0].code,"unsupported_effect");
    EXPECT_EQ(grade.diagnostics[1].code,"unsupported_parameter");
}

TEST(GradeCompilerTest, InvalidPayloadsAreErrorsEvenInPreviewAndTransactional) {
    const char* invalid[] = {
        R"({"basicGrade":{}})",
        R"([{"type":"video.exposure","enabled":1}])",
        R"([{"type":"video.exposure","params":{"exposure":2}}])",
        R"([{"type":"video.exposure","params":{"exposure":{"value":"2"}}}])",
        R"([{"type":"video.exposure","params":{"exposure":{"track":{"keyframes":[{"frame":0,"value":1},{"frame":0,"value":2}]}}}}])",
        R"([{"type":"video.exposure","params":{"exposure":{"track":{"keyframes":[{"frame":1.5,"value":1}]}}}}])",
        R"([{"type":"video.exposure","params":{"exposure":{"value":100}}}])",
        R"([{"type":"video.blur","params":{"radius":{"value":-1}}}])",
        R"([{"type":"video.exposure","params":{"exposure":{"value":1}}},{"type":"video.blur","params":{"radius":{"value":33}}}])"
    };
    for (const auto* payload : invalid) {
        auto clip = withEffects(payload);
        CompiledGrade grade;
        std::string error;
        EXPECT_FALSE(GradeCompiler::compile(clip, 0, grade, &error, {.strict=false})) << payload;
        EXPECT_TRUE(grade.neutral());
        EXPECT_FALSE(error.empty());
        EXPECT_FALSE(grade.diagnostics.empty());
    }
    auto clip = withEffects(R"([{"type":"video.exposure","params":{"exposure":{"value":1}}}])");
    (*clip.effects)[0]["params"]["exposure"]["value"] = std::numeric_limits<double>::infinity();
    CompiledGrade grade;
    EXPECT_FALSE(GradeCompiler::compile(clip, 0, grade, nullptr, {.strict=false}));
}

TEST(GradeParameterSamplerTest, DefaultEmptyTrackIntegerRoundingAndBooleanHold) {
    VulkanParameterValue sampled;
    std::vector<GradeDiagnostic> diagnostics;
    auto empty = JsonValue::parse(R"({"track":{"keyframes":[]}})");
    ASSERT_TRUE(sampleParameter(&empty, 3.0f, 0, sampled, diagnostics, "e", "p"));
    EXPECT_EQ(std::get<float>(sampled), 3.0f);
    auto numeric = JsonValue::parse(R"({"track":{"keyframes":[{"frame":0,"value":-2,"interpolationOut":"linear"},{"frame":4,"value":2}]}})");
    ASSERT_TRUE(sampleParameter(&numeric, int32_t{0}, 1, sampled, diagnostics, "e", "p"));
    EXPECT_EQ(std::get<int32_t>(sampled), -1);
    ASSERT_TRUE(sampleParameter(&numeric, int32_t{0}, 3, sampled, diagnostics, "e", "p"));
    EXPECT_EQ(std::get<int32_t>(sampled), 1);
    auto boolean = JsonValue::parse(R"({"value":true,"track":{"keyframes":[{"frame":0,"value":false,"interpolationOut":"linear"},{"frame":4,"value":true}]}})");
    ASSERT_TRUE(sampleParameter(&boolean, true, 3, sampled, diagnostics, "e", "p"));
    EXPECT_FALSE(std::get<bool>(sampled));
    ASSERT_TRUE(sampleParameter(&boolean, false, 4, sampled, diagnostics, "e", "p"));
    EXPECT_TRUE(std::get<bool>(sampled));
}

TEST(GradeParameterSamplerTest, SignedFrameExtremesDoNotOverflowLinearSampling) {
    auto payload = JsonValue::parse(R"({"track":{"keyframes":[{"frame":-9223372036854775808,"value":1,"interpolationOut":"linear"},{"frame":9223372036854775807,"value":3}]}})");
    VulkanParameterValue value;
    std::vector<GradeDiagnostic> diagnostics;
    ASSERT_TRUE(sampleParameter(&payload, 0.0f, 0, value, diagnostics, "e", "p"));
    EXPECT_FLOAT_EQ(std::get<float>(value), 2.0f);
}
