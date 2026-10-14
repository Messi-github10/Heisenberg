#include <Preview/VideoEngine.hpp>
#include <Models/Timeline.hpp>
#include <Models/MediaManifest.hpp>
#include <MultiMedia/Audio/AudioResampler.hpp>
#include <MultiMedia/DecoderContext.hpp>
#include <MultiMedia/DemuxContext.hpp>
#include <Common/Stream.hpp>
#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>
#include <thread>

using namespace heisenberg;
namespace {
constexpr double kPi = 3.14159265358979323846;
struct Capture {
    std::mutex mutex;
    std::map<uint64_t, std::vector<std::shared_ptr<const AudioFrame>>> pcm;
    std::thread::id worker;
    void put(std::shared_ptr<const AudioFrame> frame, uint64_t generation) {
        std::lock_guard lock(mutex);
        worker = std::this_thread::get_id();
        pcm[generation].push_back(std::move(frame));
    }
    std::vector<std::shared_ptr<const AudioFrame>> frames(uint64_t generation) {
        std::lock_guard lock(mutex); return pcm[generation];
    }
    std::thread::id workerId() { std::lock_guard lock(mutex); return worker; }
};
template<class Predicate> bool pumpUntil(VideoEngine& engine, Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        int wait = 0; engine.pumpFrame(wait);
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}
void configure(VideoEngine& engine, Capture& capture) {
    engine.setHardwareDecode(false);
    engine.setAudioOutputEnabled(false);
    engine.setManualClock(0.0);
    engine.onAudioPrepared = [&](std::shared_ptr<const AudioFrame> frame, uint64_t generation) {
        capture.put(std::move(frame), generation);
    };
}
double tone(const std::vector<std::shared_ptr<const AudioFrame>>& frames,
            double frequency, double begin = 0.15, double end = 0.65) {
    double real = 0, imaginary = 0; int count = 0;
    for (const auto& frame : frames) for (int s = 0; s < frame->samples(); ++s) {
        const double t = static_cast<double>(frame->position() + s) / frame->spec().sampleRate;
        if (t < begin || t >= end) continue;
        const double value = frame->sampleRo(0, s);
        real += value * std::cos(2 * kPi * frequency * t);
        imaginary += value * std::sin(2 * kPi * frequency * t); ++count;
    }
    return count ? 2 * std::hypot(real, imaginary) / count : 0;
}
MediaManifestEntry media(const std::string& id, const std::string& path, ClipType type) {
    MediaManifestEntry entry; entry.id = id; entry.name = id; entry.type = type;
    entry.source.path = path; entry.duration = 3.0; entry.hasAudio = true; return entry;
}
Clip clip(const std::string& id, const std::string& ref, ClipType type, int frames) {
    Clip value; value.id = id; value.mediaRef = ref;
    value.mediaType = value.sourceClipType = type;
    value.durationFrames = frames; return value;
}
}

TEST(IndependentAudioEngineTest, AudioOnlyFormatsPreparePcmSeekEofAndReplay) {
    const std::vector<std::string> paths{HEISENBERG_TEST_MP3, HEISENBERG_TEST_WAV,
        HEISENBERG_TEST_FLOAT_WAV, HEISENBERG_TEST_AAC, HEISENBERG_TEST_FLAC,
        HEISENBERG_TEST_OGG, HEISENBERG_TEST_COVER_MP3};
    for (const auto& path : paths) {
        SCOPED_TRACE(path);
        Capture capture; VideoEngine engine; configure(engine, capture);
        bool blank = false;
        engine.onFrameDecoded = [&](DecodedFrame frame) {
            blank = frame.blank && !frame.source;
            EXPECT_TRUE(frame.settings.valid());
        };
        ASSERT_TRUE(engine.open(path));
        ASSERT_TRUE(pumpUntil(engine, [&] { return blank && engine.snapshot().state == VideoEngine::State::Paused; }));
        auto snapshot = engine.snapshot();
        EXPECT_TRUE(snapshot.audioOnly); EXPECT_FALSE(snapshot.hasVideo);
        EXPECT_TRUE(snapshot.seekable); EXPECT_GT(snapshot.duration, 1.2);
        EXPECT_NE(capture.workerId(), std::this_thread::get_id());
        const auto originalGeneration = snapshot.generation;
        engine.play(); engine.setManualClock(10.0);
        ASSERT_TRUE(pumpUntil(engine, [&] { return engine.snapshot().state == VideoEngine::State::Ended; }));
        const auto original = capture.frames(originalGeneration);
        ASSERT_FALSE(original.empty());
        EXPECT_GT(tone(original, 880), 0.04);
        EXPECT_LT(tone(original, 440), 0.01);
        int64_t end = 0;
        for (const auto& frame : original) {
            EXPECT_EQ(frame->position(), end); end += frame->samples();
        }
        EXPECT_NEAR(static_cast<double>(end) / original.front()->spec().sampleRate, snapshot.duration, 1.0 / 48000.0);
        engine.play();
        ASSERT_TRUE(pumpUntil(engine, [&] { return engine.snapshot().generation > originalGeneration
            && engine.snapshot().state == VideoEngine::State::Playing; }));
        engine.pause(); engine.seek(0.5);
        const auto seekGeneration = engine.snapshot().generation;
        ASSERT_TRUE(pumpUntil(engine, [&] { return engine.snapshot().state == VideoEngine::State::Paused
            && !capture.frames(seekGeneration).empty(); }));
        auto sought = capture.frames(seekGeneration);
        EXPECT_GT(sought.front()->position(), 0);
        EXPECT_GT(tone(sought, 880, 0.5, 0.8), 0.04);
        engine.beginScrub(); engine.scrubToFrame(2); engine.endScrub(3);
        const auto settledGeneration = engine.snapshot().generation;
        ASSERT_TRUE(pumpUntil(engine, [&] { return engine.snapshot().state == VideoEngine::State::Paused
            && !capture.frames(settledGeneration).empty(); }));
        EXPECT_EQ(capture.frames(settledGeneration).front()->position(), 6000);
        engine.close();
    }
}

TEST(IndependentAudioEngineTest, OriginalAndIndependentTonesMixAndAudioOutlivesVideoWithoutDevice) {
    Timeline timeline; timeline.id = "mixed"; timeline.fps = 24;
    Track video; video.id = "video";
    video.clips.push_back(clip("picture", "v", ClipType::Video, 24));
    Track audio; audio.id = "audio"; audio.type = ClipType::Audio;
    audio.clips.push_back(clip("music", "a", ClipType::Audio, 72));
    timeline.tracks = {video, audio};
    MediaManifest manifest;
    ASSERT_TRUE(manifest.add(media("v", HEISENBERG_PLAYLIST_CLIP_A, ClipType::Video)));
    ASSERT_TRUE(manifest.add(media("a", HEISENBERG_TEST_LONG_AUDIO, ClipType::Audio)));
    Capture capture; VideoEngine engine; configure(engine, capture);
    bool tailBlank = false;
    engine.onFrameDecoded = [&](DecodedFrame frame) {
        if (frame.targetPosition >= 24) tailBlank = frame.blank && !frame.source;
    };
    ASSERT_TRUE(engine.openTimeline(timeline, manifest, ""));
    ASSERT_TRUE(pumpUntil(engine, [&] { return engine.snapshot().state == VideoEngine::State::Paused; }));
    const auto generation = engine.snapshot().generation;
    engine.play(); engine.setManualClock(10.0);
    ASSERT_TRUE(pumpUntil(engine, [&] { return engine.snapshot().state == VideoEngine::State::Ended; }));
    auto frames = capture.frames(generation);
    EXPECT_GT(tone(frames, 440), 0.04); EXPECT_GT(tone(frames, 880), 0.04);
    EXPECT_GT(tone(frames, 880, 1.5, 2.5), 0.04);
    EXPECT_LT(tone(frames, 440, 1.5, 2.5), 0.005);
    EXPECT_TRUE(tailBlank); EXPECT_DOUBLE_EQ(engine.snapshot().currentTime, 3.0);
    ASSERT_FALSE(frames.empty()); EXPECT_EQ(frames.back()->position() + frames.back()->samples(), 144000);
}

TEST(IndependentAudioEngineTest, SameSourceDifferentContributionOffsetsHaveSeparateDecoderState) {
    // Labeling the audio fixture as a video source deliberately exercises the
    // engine's no-video stream fallback plus two same-path contribution clocks.
    Timeline timeline; timeline.id = "offsets"; timeline.fps = 24;
    Track video; video.id = "video";
    video.clips.push_back(clip("first", "v", ClipType::Video, 24));
    Track audio; audio.id = "audio"; audio.type = ClipType::Audio;
    auto shifted = clip("shifted", "a", ClipType::Audio, 24); shifted.trimStartFrame = 1;
    audio.clips.push_back(shifted); timeline.tracks = {video, audio};
    MediaManifest manifest;
    ASSERT_TRUE(manifest.add(media("v", HEISENBERG_TEST_LONG_AUDIO, ClipType::Video)));
    ASSERT_TRUE(manifest.add(media("a", HEISENBERG_TEST_LONG_AUDIO, ClipType::Audio)));
    Capture capture; VideoEngine engine; configure(engine, capture);
    ASSERT_TRUE(engine.openTimeline(timeline, manifest, ""));
    ASSERT_TRUE(pumpUntil(engine, [&] { return engine.snapshot().state == VideoEngine::State::Paused; }));
    auto generation = engine.snapshot().generation;
    engine.play(); engine.setManualClock(10.0);
    ASSERT_TRUE(pumpUntil(engine, [&] { return engine.snapshot().state == VideoEngine::State::Ended; }));
    const auto amplitude = tone(capture.frames(generation), 880);
    // 880 Hz * 1/24 s = 36 + 2/3 cycles: the sum's amplitude is
    // one source amplitude, not the double-amplitude shared-session result.
    EXPECT_GT(amplitude, 0.06); EXPECT_LT(amplitude, 0.11);
}

TEST(IndependentAudioEngineTest, OfflineContributionsKeepDistinctStableWarnings) {
    Timeline timeline; timeline.id = "offline-audio"; timeline.fps = 24;
    Track video; video.id = "video";
    video.clips.push_back(clip("picture", "missing-file", ClipType::Video, 72));
    Track audio; audio.id = "audio"; audio.type = ClipType::Audio;
    audio.clips.push_back(clip("missing-ref", "not-in-manifest", ClipType::Audio, 72));
    timeline.tracks = {video, audio};
    MediaManifest manifest;
    ASSERT_TRUE(manifest.add(media("missing-file", std::string(HEISENBERG_TEST_LONG_AUDIO)
        + ".does-not-exist", ClipType::Video)));
    Capture capture; VideoEngine engine; configure(engine, capture);
    ASSERT_TRUE(engine.openTimeline(timeline, manifest, ""));
    ASSERT_TRUE(pumpUntil(engine, [&] { return engine.snapshot().state == VideoEngine::State::Paused; }));
    auto warnings = engine.snapshot().warnings;
    EXPECT_EQ(std::count_if(warnings.begin(), warnings.end(), [](const auto& value) {
        return value.find("unavailable media:") == 0;
    }), 1);
    auto distinct = warnings; std::sort(distinct.begin(), distinct.end());
    EXPECT_EQ(std::unique(distinct.begin(), distinct.end()), distinct.end());
    engine.play(); engine.setManualClock(10.0);
    ASSERT_TRUE(pumpUntil(engine, [&] { return engine.snapshot().state == VideoEngine::State::Ended; }));
    EXPECT_EQ(engine.snapshot().warnings, warnings);
    engine.seek(0.5);
    ASSERT_TRUE(pumpUntil(engine, [&] { return engine.snapshot().state == VideoEngine::State::Paused; }));
    EXPECT_EQ(engine.snapshot().warnings, warnings);
}

TEST(IndependentAudioEngineTest, ResamplerKeepsContinuousPositionsAndDrainsFinalSamples) {
    AudioResampler resampler;
    AudioSpec inputSpec; inputSpec.sampleRate = 44100; inputSpec.channels = 1;
    AudioSpec outputSpec; outputSpec.sampleRate = 48000; outputSpec.channels = 2;
    int64_t end = -480;
    auto check = [&](const std::shared_ptr<AudioFrame>& frame) {
        if (!frame) return;
        ASSERT_TRUE(frame->hasPosition()); EXPECT_EQ(frame->position(), end); end += frame->samples();
    };
    for (int i = 0; i < 10; ++i) {
        AudioFrame frame; frame.setSpec(inputSpec); frame.setSamples(441, true);
        frame.setPosition(-441 + i * 441);
        check(resampler.convert(frame, outputSpec));
    }
    while (auto frame = resampler.drain()) check(frame);
    EXPECT_EQ(end, 4320);
    EXPECT_FALSE(resampler.drain());
    resampler.reset();
    AudioFrame frame; frame.setSpec(inputSpec); frame.setSamples(441, true); frame.setPosition(0);
    auto restart = resampler.convert(frame, outputSpec); ASSERT_TRUE(restart); EXPECT_EQ(restart->position(), 0);
}

TEST(IndependentAudioEngineTest, DecoderRetriesEagainWithoutDroppingPacketsOrSkipSamples) {
    demuxer::DemuxContext demux; ASSERT_GE(demux.open(HEISENBERG_TEST_MP3), 0);
    const Stream* stream = nullptr;
    for (const auto& value : demux.streams()) if (value.isAudio()) stream = &value;
    ASSERT_NE(stream, nullptr);
    auto decode = [&](bool delayedPull) {
        decoder::DecoderContext decoder; EXPECT_GE(decoder.open(*stream), 0);
        EXPECT_GE(demux.seek(0, stream->index, 1), 0);
        std::vector<float> samples;
        auto pull = [&] {
            for (;;) {
                auto item = decoder.pull(1); auto frame = item.audioFrame();
                if (!frame) break;
                samples.insert(samples.end(), frame->channelRo(0), frame->channelRo(0) + frame->samples());
            }
        };
        for (;;) {
            auto packet = demux.read(1);
            if (packet.type != MediaFrameType::Packet) break;
            if (packet.metadata.streamIndex == stream->index) EXPECT_TRUE(decoder.push(packet));
            if (!delayedPull) pull();
        }
        EXPECT_TRUE(decoder.push(MediaFrame::eof(1))); pull(); return samples;
    };
    auto immediate = decode(false); auto delayed = decode(true);
    ASSERT_GT(immediate.size(), 50000u); ASSERT_EQ(immediate.size(), delayed.size());
    EXPECT_EQ(immediate, delayed);
}
