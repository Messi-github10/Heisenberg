#include <Models/Timeline.hpp>
#include <Models/MediaManifest.hpp>
#include <Preview/VideoEngine.hpp>
#include <Compositing/CompositionBuilder.hpp>
#include <Compositing/FrameRenderer.hpp>

#include <Models/ProjectCodec.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <chrono>
#include <thread>
#include <optional>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace {

using json = nlohmann::json;

heisenberg::Clip clip(std::string id, std::string mediaRef, int64_t start, int64_t duration) {
    heisenberg::Clip value;
    value.id = std::move(id);
    value.mediaRef = std::move(mediaRef);
    value.startFrame = start;
    value.durationFrames = duration;
    return value;
}

heisenberg::Timeline timelineWithTrack(std::vector<heisenberg::Clip> clips) {
    heisenberg::Timeline timeline;
    timeline.id = "59F931DC-0A80-4B90-9A20-A1330C623504";
    timeline.fps = 24;
    timeline.width = 320;
    timeline.height = 240;
    heisenberg::Track track;
    track.id = "7BA50613-7748-49E4-A8E7-95B004747B3B";
    track.clips = std::move(clips);
    timeline.tracks.push_back(std::move(track));
    return timeline;
}

bool isBlackCanvas(const std::shared_ptr<heisenberg::VideoFrame>& frame) {
    if (!frame || !frame->get() || frame->spec().format != AV_PIX_FMT_RGBAF16 || !frame->get()->data[0]) return false;
    const auto* pixel = reinterpret_cast<const uint16_t*>(frame->get()->data[0]);
    return pixel[0] == 0 && pixel[1] == 0 && pixel[2] == 0 && pixel[3] == 0x3C00;
}

std::optional<heisenberg::DecodedFrame> pumpUntilFrame(heisenberg::VideoEngine& engine, int64_t position) {
    std::optional<heisenberg::DecodedFrame> output;
    engine.onFrameDecoded = [&](heisenberg::DecodedFrame frame) {
        if (frame.targetPosition == position) output = std::move(frame);
    };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!output && std::chrono::steady_clock::now() < deadline) {
        int waitMs = 0;
        engine.pumpFrame(waitMs);
        if (!output) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    engine.onFrameDecoded = {};
    return output;
}

} // namespace

TEST(ProjectCodecTest, FullPalmierDocumentRoundTripsByJsonDom) {
    const std::string fixture = R"json({
      "timelines":[{"id":"59F931DC-0A80-4B90-9A20-A1330C623504","name":"Main","fps":30,"width":1920,"height":1080,"settingsConfigured":true,"folderId":"folder-timeline","tracks":[{"id":"7BA50613-7748-49E4-A8E7-95B004747B3B","type":"video","name":"V1","muted":false,"hidden":false,"syncLocked":true,"clips":[{"id":"59F6E869-D942-4A5D-8F10-8C1B5E743BEC","mediaRef":"media-a","mediaType":"video","sourceClipType":"video","startFrame":4,"durationFrames":20,"trimStartFrame":3,"trimEndFrame":7,"speed":1.25,"volume":0.8,"fadeInFrames":2,"fadeOutFrames":3,"fadeInInterpolation":"smooth","fadeOutInterpolation":"linear","opacity":0.75,"transform":{"centerX":0.4,"centerY":0.6,"width":0.8,"height":0.7,"rotation":12.0,"rotationX":2.0,"rotationY":3.0,"flipHorizontal":true,"flipVertical":false},"crop":{"left":0.1,"top":0.2,"right":0.05,"bottom":0.0},"edgeRounding":0.2,"edgeSoftness":0.1,"linkGroupId":"link-a","captionGroupId":"caption-a","multicamGroupId":"multi-a","textContent":"hello","textStyle":{"fontScale":1.2},"textAnimation":{"type":"fade"},"wordTimings":[{"text":"hello","startFrame":1,"endFrame":4}],"textFillMode":{"solid":{}},"opacityTrack":{"keyframes":[{"frame":2,"value":0.5,"interpolationOut":"smooth"}]},"positionTrack":{"keyframes":[{"frame":3,"value":{"a":0.2,"b":0.3},"interpolationOut":"linear"}]},"scaleTrack":{"keyframes":[]},"rotationTrack":{"keyframes":[{"frame":5,"value":15.0,"interpolationOut":"hold"}]},"cropTrack":{"keyframes":[{"frame":6,"value":{"left":0.1,"top":0.0,"right":0.0,"bottom":0.0},"interpolationOut":"smooth"}]},"volumeTrack":{"keyframes":[{"frame":7,"value":-3.0,"interpolationOut":"linear"}]},"effects":[{"id":"effect-a","type":"video.blur","enabled":true,"params":{"radius":{"value":4.0,"track":{"keyframes":[{"frame":3,"value":5.0,"interpolationOut":"smooth"}]}}}}],"blendMode":"multiply"}],"displayHeight":72.0}],"markers":[{"id":"marker-a","name":"Review","startFrame":8,"durationFrames":4,"color":{"r":1.0,"g":0.5,"b":0.0,"a":1.0},"comment":"check","status":"review"}]}],
      "activeTimelineId":"59F931DC-0A80-4B90-9A20-A1330C623504",
      "openTimelineIds":["59F931DC-0A80-4B90-9A20-A1330C623504"],
      "viewStates":{"59F931DC-0A80-4B90-9A20-A1330C623504":{"playheadFrame":12,"zoomScale":3.5,"scrollOffsetX":40.0}},
      "speakers":[{"id":1,"name":"Speaker 1","color":[0.1,0.2,0.3,1.0],"centroid":[0.5,0.25]}],
      "multicamGroups":[{"id":"multi-a","name":"Interview","members":[{"id":"member-a","mediaRef":"media-a","kind":"both","angleLabel":"A","sync":{"offsetSeconds":0.25,"confidence":0.9,"locked":true}}],"masterMemberId":"member-a"}]
    })json";

    heisenberg::ProjectFile project;
    std::string error;
    ASSERT_TRUE(heisenberg::ProjectCodec::decode(fixture, project, &error)) << error;
    EXPECT_EQ(project.timelines.front().totalFrames(), 24);
    const json before = json::parse(fixture);
    const json after = json::parse(heisenberg::ProjectCodec::encode(project));
    EXPECT_EQ(after, before);
}

TEST(ProjectCodecTest, RejectsLegacyHeisenbergV1Root) {
    const std::string legacy = R"json({"version":1,"profile":{"id":"hd"},"clips":[],"filters":[]})json";
    heisenberg::ProjectFile project;
    std::string error;
    EXPECT_FALSE(heisenberg::ProjectCodec::decode(legacy, project, &error));
    EXPECT_NE(error.find("timelines"), std::string::npos);
}

TEST(MediaManifestTest, VersionTwoSourceDatesAndFoldersRoundTrip) {
    const std::string fixture = R"json({"version":2,"entries":[{"id":"media-a","name":"A","type":"video","source":{"external":{"absolutePath":"C:/media/a.mp4"}},"duration":1.5,"generationInput":{"prompt":"p","createdAt":42.25},"sourceWidth":1920,"sourceHeight":1080,"sourceFPS":30.0,"hasAudio":true,"folderId":"folder-a","cachedRemoteURL":"https://example.test/a","cachedRemoteURLExpiresAt":123.5,"generationStatus":"ready","importInput":{"sourcePath":"C:/media/a.mp4","createdAt":22.0}},{"id":"media-b","name":"B","type":"image","source":{"project":{"relativePath":"media/b.png"}},"duration":0.0}],"folders":[{"id":"folder-a","name":"Shots","parentFolderId":"root"}]})json";
    heisenberg::MediaManifest manifest;
    std::string error;
    ASSERT_TRUE(manifest.loadFromJson(fixture, &error)) << error;
    ASSERT_EQ(manifest.entries().size(), 2u);
    EXPECT_DOUBLE_EQ(manifest.entries()[0].duration, 1.5);
    EXPECT_EQ(json::parse(manifest.toJson()), json::parse(fixture));
}

TEST(TimelineModelTest, UsesHalfOpenRangesAndMaximumTrackEnd) {
    heisenberg::Timeline timeline = timelineWithTrack({clip("a", "m", 2, 3), clip("b", "m", 8, 2)});
    heisenberg::Track audio;
    audio.id = "78209F0B-BD9E-4FEC-99E0-2F04C11B55AD";
    audio.type = heisenberg::ClipType::Audio;
    audio.clips.push_back(clip("c", "audio", 1, 12));
    audio.clips.front().mediaType = heisenberg::ClipType::Audio;
    audio.clips.front().sourceClipType = heisenberg::ClipType::Audio;
    timeline.tracks.push_back(audio);
    EXPECT_FALSE(timeline.tracks[0].clips[0].contains(1));
    EXPECT_TRUE(timeline.tracks[0].clips[0].contains(2));
    EXPECT_TRUE(timeline.tracks[0].clips[0].contains(4));
    EXPECT_FALSE(timeline.tracks[0].clips[0].contains(5));
    EXPECT_EQ(timeline.totalFrames(), 13);
}

TEST(TimelineModelTest, IndependentTrackLookupAndCompatibilityRejectOnlyUnsupportedLayouts) {
    auto timeline = timelineWithTrack({clip("video-clip", "video", 0, 24)});
    heisenberg::Track audio;
    audio.id = "audio-track"; audio.type = heisenberg::ClipType::Audio;
    auto sound = clip("audio-clip", "audio", 0, 48);
    sound.mediaType = sound.sourceClipType = heisenberg::ClipType::Audio;
    audio.clips.push_back(sound);
    timeline.tracks.push_back(audio);
    EXPECT_TRUE(heisenberg::validateTimeline(timeline)); // Cross-track overlap is valid.
    EXPECT_EQ(heisenberg::compatibilityFor(timeline).level, heisenberg::CompatibilityLevel::Supported);
    EXPECT_EQ(timeline.trackForClip("audio-clip"), timeline.trackById("audio-track"));
    const auto& immutable = timeline;
    EXPECT_EQ(immutable.trackForClip("audio-clip"), immutable.trackById("audio-track"));
    EXPECT_EQ(immutable.trackForClip("missing"), nullptr);
    EXPECT_EQ(immutable.trackById("missing"), nullptr);
    timeline.tracks.back().clips.front().sourceClipType = heisenberg::ClipType::Video;
    EXPECT_EQ(heisenberg::compatibilityFor(timeline).level, heisenberg::CompatibilityLevel::Unsupported);
    timeline.tracks.back().clips.front().sourceClipType = heisenberg::ClipType::Audio;
    auto duplicate = audio; duplicate.id = "duplicate-audio"; duplicate.clips.clear();
    timeline.tracks.push_back(duplicate);
    EXPECT_EQ(heisenberg::compatibilityFor(timeline).level, heisenberg::CompatibilityLevel::Unsupported);
}

TEST(ProjectCodecTest, IndependentAudioUsesExistingPalmierWireAndRoundTripsAutomation) {
    const std::string fixture = R"json({"timelines":[{"id":"timeline","fps":24,"width":320,"height":240,"tracks":[{"id":"video","type":"video","clips":[{"id":"picture","mediaRef":"video-media","startFrame":0,"durationFrames":24}]},{"id":"audio","type":"audio","muted":true,"clips":[{"id":"sound","mediaRef":"audio-media","mediaType":"audio","sourceClipType":"audio","startFrame":3,"durationFrames":48,"volume":1.5,"fadeInFrames":4,"fadeOutFrames":5,"fadeInInterpolation":"smooth","fadeOutInterpolation":"hold","volumeTrack":{"keyframes":[{"frame":2,"value":0.2,"interpolationOut":"smooth"}]}}]}]}],"activeTimelineId":"timeline"})json";
    heisenberg::ProjectFile project;
    std::string error;
    ASSERT_TRUE(heisenberg::ProjectCodec::decode(fixture, project, &error)) << error;
    ASSERT_EQ(project.timelines.size(), 1u);
    EXPECT_EQ(project.timelines.front().totalFrames(), 51);
    EXPECT_EQ(heisenberg::compatibilityFor(project.timelines.front()).level, heisenberg::CompatibilityLevel::Warning);
    const auto& video = project.timelines.front().tracks.front().clips.front();
    EXPECT_EQ(video.mediaType, heisenberg::ClipType::Video); // Legacy omitted fields still default to video.
    EXPECT_EQ(video.sourceClipType, heisenberg::ClipType::Video);
    const auto encoded = heisenberg::ProjectCodec::encode(project);
    const auto object = json::parse(encoded);
    EXPECT_FALSE(object.contains("version"));
    EXPECT_EQ(object["timelines"][0]["tracks"][1]["type"], "audio");
    EXPECT_EQ(object["timelines"][0]["tracks"][1]["clips"][0]["fadeOutInterpolation"], "hold");
    heisenberg::ProjectFile reopened;
    ASSERT_TRUE(heisenberg::ProjectCodec::decode(encoded, reopened, &error)) << error;
    EXPECT_EQ(reopened, project);
}

TEST(TimelineModelTest, ValidationRejectsOverlapDuplicateIdsAndOverflow) {
    heisenberg::Timeline timeline = timelineWithTrack({clip("same", "m", 0, 5), clip("same", "m", 4, 2)});
    std::string error;
    EXPECT_FALSE(heisenberg::validateTimeline(timeline, &error));
    timeline.tracks[0].clips[1].id = "other";
    timeline.tracks[0].clips[1].startFrame = std::numeric_limits<int64_t>::max() - 1;
    timeline.tracks[0].clips[1].durationFrames = 2;
    EXPECT_FALSE(heisenberg::validateTimeline(timeline, &error));
}

TEST(VideoEngineTimelineTest, GapsAndOfflineMediaProduceBlackFramesAndWarnings) {
    heisenberg::Timeline timeline = timelineWithTrack({clip("a", "missing", 3, 2)});
    heisenberg::MediaManifest manifest;
    heisenberg::MediaManifestEntry entry;
    entry.id = "missing";
    entry.name = "Missing";
    entry.source = {heisenberg::MediaSource::Kind::External, "C:/definitely-not-here.mp4"};
    entry.duration = 1.0;
    ASSERT_TRUE(manifest.add(entry));
    heisenberg::CompositionPlan plan;
    std::string error;
    ASSERT_TRUE(heisenberg::CompositionBuilder::build(timeline, manifest, {}, plan, &error)) << error;
    ASSERT_EQ(plan.frameCount, 5);
    // Building captures paths without probing disk availability.
    EXPECT_TRUE(plan.warnings.empty());
    EXPECT_FALSE(plan.sampleAt(0));
    heisenberg::VideoEngine engine;
    engine.setHardwareDecode(false);
    ASSERT_TRUE(engine.openTimeline(timeline, manifest, {}));
    const auto gap = pumpUntilFrame(engine, 0);
    ASSERT_TRUE(gap);
    EXPECT_TRUE(gap->blank);
    heisenberg::FrameRenderer renderer(gap->settings);
    EXPECT_TRUE(isBlackCanvas(renderer.background(gap->targetPosition, &error))) << error;
    engine.seek(3.0 / timeline.fps);
    const auto offline = pumpUntilFrame(engine, 3);
    ASSERT_TRUE(offline);
    EXPECT_TRUE(offline->blank);
    EXPECT_FALSE(engine.snapshot().warnings.empty());
    EXPECT_TRUE(isBlackCanvas(renderer.background(offline->targetPosition, &error))) << error;
    engine.seek(99.0);
    ASSERT_TRUE(pumpUntilFrame(engine, 4));
    EXPECT_DOUBLE_EQ(engine.snapshot().currentTime, 4.0 / timeline.fps);
    engine.play();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (engine.snapshot().state != heisenberg::VideoEngine::State::Ended &&
           std::chrono::steady_clock::now() < deadline) {
        int waitMs = 0;
        engine.pumpFrame(waitMs);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT_EQ(engine.snapshot().state, heisenberg::VideoEngine::State::Ended);
}

TEST(VideoEngineTimelineTest, ReadsRealMediaAcrossCutWithTrimAndSpeed) {
    heisenberg::Timeline timeline = timelineWithTrack({clip("a", "media-a", 0, 4), clip("b", "media-a", 4, 3)});
    timeline.tracks[0].clips[0].trimStartFrame = 2;
    timeline.tracks[0].clips[1].trimStartFrame = 4;
    timeline.tracks[0].clips[1].speed = 2.0;
    heisenberg::MediaManifest manifest;
    heisenberg::MediaManifestEntry entry;
    entry.id = "media-a";
    entry.name = "A";
    entry.source = {heisenberg::MediaSource::Kind::External, HEISENBERG_PLAYLIST_CLIP_A};
    entry.duration = 1.0;
    ASSERT_TRUE(manifest.add(entry));
    heisenberg::CompositionPlan plan;
    std::string error;
    ASSERT_TRUE(heisenberg::CompositionBuilder::build(timeline, manifest, {}, plan, &error)) << error;
    ASSERT_EQ(plan.frameCount, 7);
    ASSERT_TRUE(plan.sampleAt(0));
    EXPECT_DOUBLE_EQ(plan.sampleAt(0)->sourceSeconds, 2.0 / timeline.fps);
    EXPECT_DOUBLE_EQ(plan.sampleAt(3)->sourceSeconds, 5.0 / timeline.fps);
    EXPECT_DOUBLE_EQ(plan.sampleAt(4)->sourceSeconds, 4.0 / timeline.fps);
    EXPECT_DOUBLE_EQ(plan.sampleAt(6)->sourceSeconds, 8.0 / timeline.fps);
    heisenberg::VideoEngine engine;
    engine.setHardwareDecode(false);
    ASSERT_TRUE(engine.openTimeline(timeline, manifest, {}));
    for (int64_t frame = 0; frame < plan.frameCount; ++frame) {
        if (frame > 0) engine.seek(static_cast<double>(frame) / timeline.fps);
        const auto value = pumpUntilFrame(engine, frame);
        ASSERT_TRUE(value) << frame;
        EXPECT_FALSE(value->blank) << frame;
        ASSERT_TRUE(value->source) << frame;
        EXPECT_EQ(value->targetPosition, frame);
        EXPECT_EQ(value->targetTimeBase, timeline.settings().timeBase());
    }
}
