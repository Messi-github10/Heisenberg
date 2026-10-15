#include <Compositing/CompositionExecutor.hpp>
#include <Preview/VideoEngine.hpp>
#include <MultiMedia/DemuxContext.hpp>
#include <MultiMedia/DecoderContext.hpp>
#include <Common/Packet.hpp>
#include <Common/Stream.hpp>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}
#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <thread>
#include <type_traits>

using namespace heisenberg;
namespace {
constexpr double kPi = 3.14159265358979323846;
struct Wave {
    std::filesystem::path path;
    int rate;
    int count;
    explicit Wave(int sampleRate = 44100, int samples = 88200) : rate(sampleRate), count(samples) {
        static std::atomic<unsigned> sequence{0};
        path = std::filesystem::temp_directory_path() / ("heisenberg-executor-"
            + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())) + "-"
            + std::to_string(sequence.fetch_add(1)) + ".wav");
        std::ofstream stream(path, std::ios::binary);
        auto integer = [&](uint32_t value, int bytes) {
            for (int byte = 0; byte < bytes; ++byte) stream.put(static_cast<char>((value >> (8 * byte)) & 255));
        };
        stream.write("RIFF", 4); integer(36 + count * 2, 4); stream.write("WAVEfmt ", 8);
        integer(16, 4); integer(1, 2); integer(1, 2); integer(rate, 4); integer(rate * 2, 4);
        integer(2, 2); integer(16, 2); stream.write("data", 4); integer(count * 2, 4);
        for (int sample = 0; sample < count; ++sample) {
            const auto value = static_cast<int16_t>(std::lround(9000 * std::sin(2 * kPi * 613 * sample / rate)
                + 3000 * std::cos(2 * kPi * 997 * sample / rate)));
            integer(static_cast<uint16_t>(value), 2);
        }
    }
    ~Wave() { std::error_code error; std::filesystem::remove(path, error); }
    std::string name() const { return path.string(); }
};
CompositionPlan audioPlan(const std::string& path, int64_t frames = 48) {
    CompositionPlan plan;
    plan.frameCount = frames;
    CompositionClip clip;
    clip.id = "audio"; clip.mediaRef = "asset"; clip.sourcePath = path; clip.durationFrames = frames;
    plan.audioClips = {clip};
    return plan;
}
void equalRange(const AudioFrame& expected, const AudioFrame& actual) {
    ASSERT_EQ(expected.spec(), actual.spec());
    ASSERT_GE(actual.position(), expected.position());
    ASSERT_LE(actual.position() + actual.samples(), expected.position() + expected.samples());
    const int offset = static_cast<int>(actual.position() - expected.position());
    for (int channel = 0; channel < actual.spec().channels; ++channel)
        for (int sample = 0; sample < actual.samples(); ++sample) {
            if (expected.sampleRo(channel, offset + sample) != actual.sampleRo(channel, sample)) {
                ADD_FAILURE() << "PCM differs at absolute sample " << actual.position() + sample
                    << ", channel " << channel;
                return;
            }
        }
}
double energy(const AudioFrame& frame) {
    double value = 0;
    for (int sample = 0; sample < frame.samples(); ++sample) value += std::abs(frame.sampleRo(0, sample));
    return value / std::max(1, frame.samples());
}
}

TEST(CompositionExecutorTest, IndependentInstancesAreSynchronousWithoutDeviceOrWindow) {
    static_assert(std::is_same_v<DecodedFrame, CompositionVideoSample>);
    Wave wave;
    auto plan = audioPlan(wave.name());
    CompositionExecutor left, right;
    ASSERT_TRUE(left.open(plan, {})); ASSERT_TRUE(right.open(plan, {}));
    auto leftVideo = left.videoFrame(0, {4});
    ASSERT_TRUE(leftVideo); EXPECT_TRUE(leftVideo.value.blank); EXPECT_FALSE(leftVideo.value.source);
    ASSERT_TRUE(leftVideo.value.plan);
    plan.audioClips.clear();
    EXPECT_EQ(leftVideo.value.plan->audioClips.size(), 1u);
    // Independent instances may execute concurrently, but neither creates a
    // callback/worker/device. Results are available before each call returns.
    CompositionExecutor::AudioResult other;
    std::thread thread([&] { other = right.audioSamples(0, 8000, {9}); });
    auto first = left.audioSamples(0, 8000, {4});
    thread.join();
    ASSERT_TRUE(first); ASSERT_TRUE(other); equalRange(*first.value, *other.value);
    EXPECT_GT(energy(*first.value), 0.05);
    left.close();
    EXPECT_EQ(left.videoFrame(0).status, CompositionStatus::Failure);
    EXPECT_TRUE(right.videoFrame(1));
    EXPECT_EQ(leftVideo.value.plan->audioClips.front().id, "audio");
}

TEST(CompositionExecutorTest, RealResampledPcmIsBitIdenticalAcrossChunksBackwardSeekAndGeneration) {
    Wave wave;
    auto plan = audioPlan(wave.name(), 36);
    auto& clip = plan.audioClips.front();
    clip.trimStartFrame = 3; clip.speed = 0.75; clip.volume = 0.8;
    clip.fadeInFrames = 4; clip.fadeOutFrames = 7;
    CompositionExecutor whole, sliced;
    ASSERT_TRUE(whole.open(plan, {})); ASSERT_TRUE(sliced.open(plan, {}));
    const auto total = whole.info().audioSampleCount;
    auto reference = whole.audioSamples(0, total);
    ASSERT_TRUE(reference); ASSERT_EQ(reference.value->samples(), total);
    int64_t cursor = 0;
    for (const int size : {1, 37, 2047, 4099, 73, 10000, 1801, 1, 32768, 100000}) {
        if (cursor >= total) break;
        auto block = sliced.audioSamples(cursor, std::min(total, cursor + size));
        ASSERT_TRUE(block); equalRange(*reference.value, *block.value);
        cursor += block.value->samples();
    }
    ASSERT_EQ(cursor, total);
    for (const int64_t position : {int64_t{1}, int64_t{42001}, int64_t{2000}, total - 701}) {
        auto sought = sliced.audioSamples(position, std::min(total, position + 700));
        ASSERT_TRUE(sought); equalRange(*reference.value, *sought.value);
    }
    auto freshGeneration = sliced.audioSamples(43001, 44517, {99});
    ASSERT_TRUE(freshGeneration); equalRange(*reference.value, *freshGeneration.value);
}

TEST(CompositionExecutorTest, CutsGapsAndSamePathContributionIdentitiesDoNotLeak) {
    Wave wave;
    auto plan = audioPlan(wave.name(), 24);
    plan.audioClips.front().startFrame = 5;
    plan.audioClips.front().durationFrames = 19;
    plan.audioClips.front().trimStartFrame = 2;
    plan.audioClips.front().volume = 0.2;
    CompositionClip original = plan.audioClips.front();
    original.id = "original"; original.mediaRef = "original-asset";
    original.startFrame = 0; original.durationFrames = 12; original.trimStartFrame = 0;
    original.speed = 1.3; original.volume = 0.3;
    plan.clips = {original};
    CompositionExecutor mixed, videoOnly, audioOnly;
    ASSERT_TRUE(mixed.open(plan, {}));
    auto onlyVideo = plan; onlyVideo.audioClips.clear();
    auto onlyAudio = plan; onlyAudio.clips.clear();
    ASSERT_TRUE(videoOnly.open(onlyVideo, {})); ASSERT_TRUE(audioOnly.open(onlyAudio, {}));
    auto all = mixed.audioSamples(0, 48000);
    auto video = videoOnly.audioSamples(0, 48000);
    auto audio = audioOnly.audioSamples(0, 48000);
    ASSERT_TRUE(all); ASSERT_TRUE(video); ASSERT_TRUE(audio);
    for (int sample = 0; sample < all.value->samples(); ++sample) {
        EXPECT_FLOAT_EQ(all.value->sampleRo(0, sample),
            video.value->sampleRo(0, sample) + audio.value->sampleRo(0, sample));
        if (sample < 10000) EXPECT_EQ(audio.value->sampleRo(0, sample), 0.0f);
        if (sample >= 24000) EXPECT_EQ(video.value->sampleRo(0, sample), 0.0f);
    }
    auto image = mixed.videoFrame(7, {8});
    ASSERT_TRUE(image); EXPECT_EQ(image.value.clipId, "original");
    EXPECT_EQ(image.value.mediaRef, "original-asset"); EXPECT_EQ(image.value.clipLocalFrame, 7);
    EXPECT_TRUE(image.value.blank); // An audio-only source is valid in this lane.
}

TEST(CompositionExecutorTest, AudioTailAndFinalResamplerDrainAreRetained) {
    Wave wave(44100, 44100);
    CompositionExecutor source;
    auto opened = source.openSource(wave.name(), {});
    ASSERT_TRUE(opened); EXPECT_FALSE(opened.value.hasVideo); EXPECT_TRUE(opened.value.hasAudio);
    ASSERT_EQ(opened.value.audioSampleCount, 48000);
    auto pcm = source.audioSamples(0, 48001);
    ASSERT_TRUE(pcm); EXPECT_EQ(pcm.value->samples(), 48000);
    auto last = source.audioSamples(47950, 48000);
    ASSERT_TRUE(last); equalRange(*pcm.value, *last.value);
    EXPECT_GT(energy(*last.value), 0.05);
    EXPECT_EQ(source.audioSamples(48000, 48001).status, CompositionStatus::End);
    EXPECT_EQ(source.videoFrame(opened.value.frameCount).status, CompositionStatus::End);

    auto plan = audioPlan(wave.name(), 24);
    plan.clips = plan.audioClips;
    plan.clips.front().id = "short-video"; plan.clips.front().durationFrames = 8;
    CompositionExecutor executor; ASSERT_TRUE(executor.open(plan, {}));
    auto tailVideo = executor.videoFrame(20); ASSERT_TRUE(tailVideo); EXPECT_TRUE(tailVideo.value.blank);
    auto tail = executor.audioSamples(40000, 48000); ASSERT_TRUE(tail); EXPECT_GT(energy(*tail.value), 0.05);
    EXPECT_EQ(tail.value->position() + tail.value->samples(), 48000);
}

TEST(CompositionExecutorTest, StrictFailurePreviewFallbackAndCancellationAreDistinct) {
    auto plan = audioPlan("heisenberg-deliberately-missing-source.wav", 24);
    CompositionExecutor strict, preview;
    ASSERT_TRUE(strict.open(plan, {}));
    auto failed = strict.audioSamples(0, 1000);
    EXPECT_EQ(failed.status, CompositionStatus::Failure); EXPECT_FALSE(failed.error.empty()); EXPECT_FALSE(failed.value);
    CompositionExecutor::Options options; options.errorPolicy = CompositionErrorPolicy::Preview;
    ASSERT_TRUE(preview.open(plan, options));
    auto silence = preview.audioSamples(0, 1000); ASSERT_TRUE(silence); EXPECT_EQ(energy(*silence.value), 0.0);
    ASSERT_EQ(preview.warnings().size(), 1u);
    auto warnings = preview.warnings(); ASSERT_TRUE(preview.audioSamples(1000, 2000)); EXPECT_EQ(preview.warnings(), warnings);
    EXPECT_EQ(strict.audioSamples(0, 1000, {1, [] { return true; }}).status, CompositionStatus::Cancelled);
    EXPECT_EQ(strict.open(plan, {}, {1, [] { return true; }}).status, CompositionStatus::Cancelled);

    Wave wave;
    CompositionExecutor executor, reference;
    ASSERT_TRUE(executor.openSource(wave.name(), {})); ASSERT_TRUE(reference.openSource(wave.name(), {}));
    int polls = 0;
    auto interrupted = executor.audioSamples(0, 96000, {5, [&] { return ++polls > 12; }});
    EXPECT_EQ(interrupted.status, CompositionStatus::Cancelled); EXPECT_FALSE(interrupted.value);
    auto resumed = executor.audioSamples(20001, 26001, {6});
    auto expected = reference.audioSamples(20001, 26001, {6});
    ASSERT_TRUE(resumed); ASSERT_TRUE(expected); equalRange(*expected.value, *resumed.value);
    // The temporary callable above must not be retained by a surviving demux.
    EXPECT_TRUE(executor.audioSamples(26001, 27001, {6}));
}

TEST(CompositionExecutorTest, DemuxInterruptDoesNotMasqueradeAsEof) {
    Wave wave;
    demuxer::DemuxContext demux;
    ASSERT_GE(demux.open(wave.name()), 0);
    demux.setInterrupt([] { return true; });
    auto interrupted = demux.readResult(1);
    EXPECT_EQ(interrupted.status, demuxer::ReadStatus::Cancelled);
    EXPECT_NE(interrupted.frame.type, MediaFrameType::Eof);
    demux.setInterrupt({});
    EXPECT_EQ(demux.readResult(2).status, demuxer::ReadStatus::Packet);
    demux.close();
    EXPECT_EQ(demux.readResult(3).status, demuxer::ReadStatus::Failure);
}

TEST(CompositionExecutorTest, InvalidFfmpegPacketIsFatalNotEagainOrEnd) {
    Stream stream;
    stream.index = 0; stream.type = Stream::VIDEO;
    stream.codec.type = CodecParams::VIDEO;
    stream.codec.nativeCodecId = AV_CODEC_ID_MPEG4;
    stream.codec.video.spec.width = 32; stream.codec.video.spec.height = 32;
    stream.codec.timeBase = Rational(1, 24);
    decoder::DecoderContext decoder;
    ASSERT_GE(decoder.open(stream), 0);
    EXPECT_EQ(decoder.pull(1).type, MediaFrameType::None); // normal EAGAIN
    EXPECT_EQ(decoder.lastError(), 0);
    // MPEG4 has no valid VOP/header in this invalid elementary packet.
    // FFmpeg may reject send or report it on pull.
    auto packet = std::make_shared<Packet>(128);
    std::fill_n(packet->data(), packet->size(), uint8_t{0x55});
    packet->streamIndex = 0; packet->timeBaseNum = 1; packet->timeBaseDen = 24;
    const bool accepted = decoder.push(MediaFrame::packet(packet, 1));
    if (accepted) {
        decoder.pull(1);
        if (decoder.lastError() == 0) {
            decoder.push(MediaFrame::eof(1)); decoder.pull(1);
        }
    }
    EXPECT_LT(decoder.lastError(), 0);
    EXPECT_NE(decoder.lastError(), AVERROR(EAGAIN)); EXPECT_NE(decoder.lastError(), AVERROR_EOF);
    EXPECT_NE(decoder.pull(1).type, MediaFrameType::Eof);
    decoder.flush(); EXPECT_EQ(decoder.lastError(), 0);
}

TEST(CompositionExecutorTest, DemuxedDamagedPacketFailsStrictAndWarnsInPreview) {
    Wave fixture;
    const auto path = fixture.path.string() + ".avi";
    struct Cleanup {
        std::string path;
        ~Cleanup() { std::error_code error; std::filesystem::remove(path, error); }
    } cleanup{path};
    AVFormatContext* format = nullptr;
    ASSERT_GE(avformat_alloc_output_context2(&format, nullptr, "avi", path.c_str()), 0);
    ASSERT_NE(format, nullptr);
    struct FormatCleanup {
        AVFormatContext* format;
        ~FormatCleanup() { if (format->pb) avio_closep(&format->pb); avformat_free_context(format); }
    } formatCleanup{format};
    auto* stream = avformat_new_stream(format, nullptr);
    ASSERT_NE(stream, nullptr);
    stream->time_base = AVRational{1, 24};
    stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    stream->codecpar->codec_id = AV_CODEC_ID_MPEG4;
    stream->codecpar->width = 32; stream->codecpar->height = 32;
    ASSERT_GE(avio_open(&format->pb, path.c_str(), AVIO_FLAG_WRITE), 0);
    ASSERT_GE(avformat_write_header(format, nullptr), 0);
    AVPacket* raw = av_packet_alloc(); ASSERT_NE(raw, nullptr);
    ASSERT_GE(av_new_packet(raw, 128), 0);
    std::fill_n(raw->data, raw->size, uint8_t{0x55});
    raw->pts = raw->dts = 0; raw->duration = 1; raw->stream_index = stream->index; raw->flags = AV_PKT_FLAG_KEY;
    EXPECT_GE(av_write_frame(format, raw), 0); av_packet_free(&raw);
    ASSERT_GE(av_write_trailer(format), 0); ASSERT_GE(avio_closep(&format->pb), 0);
    CompositionPlan plan; plan.frameCount = 1;
    CompositionClip clip; clip.sourcePath = path; clip.durationFrames = 1; plan.clips = {clip};
    CompositionExecutor strict, preview;
    ASSERT_TRUE(strict.open(plan, {}));
    auto failed = strict.videoFrame(0);
    EXPECT_EQ(failed.status, CompositionStatus::Failure); EXPECT_FALSE(failed.error.empty());
    CompositionExecutor::Options options; options.errorPolicy = CompositionErrorPolicy::Preview;
    ASSERT_TRUE(preview.open(plan, options));
    auto blank = preview.videoFrame(0); ASSERT_TRUE(blank); EXPECT_TRUE(blank.value.blank);
    EXPECT_FALSE(preview.warnings().empty());
}

#ifdef HEISENBERG_TEST_MP3
TEST(CompositionExecutorTest, CompressedPrimingAndRandomSeekKeepSamePcmAsSequentialDecode) {
    for (const std::string path : {std::string(HEISENBERG_TEST_MP3), std::string(HEISENBERG_TEST_AAC)}) {
        SCOPED_TRACE(path);
        CompositionExecutor sequential, random;
        ASSERT_TRUE(sequential.openSource(path, {})); ASSERT_TRUE(random.openSource(path, {}));
        const auto count = std::min<int64_t>(sequential.info().audioSampleCount, 96000);
        auto baseline = sequential.audioSamples(0, count); ASSERT_TRUE(baseline);
        const int64_t begin = std::min<int64_t>(24137, count / 2);
        const int64_t end = std::min(count, begin + 17003);
        auto sought = random.audioSamples(begin, end, {11}); ASSERT_TRUE(sought);
        equalRange(*baseline.value, *sought.value);
        auto backward = random.audioSamples(31, std::min<int64_t>(count, 1531), {11}); ASSERT_TRUE(backward);
        equalRange(*baseline.value, *backward.value);
    }
}
#endif

#ifdef HEISENBERG_TEST_MEDIA
TEST(CompositionExecutorTest, VideoNativePtsAndOwnedClipPlanSurviveIndependentReuse) {
    CompositionExecutor executor;
    CompositionPlan plan; plan.frameCount = 24;
    CompositionClip clip; clip.id = "picture"; clip.mediaRef = "media";
    clip.sourcePath = HEISENBERG_TEST_MEDIA; clip.startFrame = 3; clip.durationFrames = 15; clip.trimStartFrame = 2;
    plan.clips = {clip};
    ASSERT_TRUE(executor.open(plan, {}));
    auto result = executor.videoFrame(7, {3}); ASSERT_TRUE(result); ASSERT_TRUE(result.value.source);
    EXPECT_EQ(result.value.clipId, "picture"); EXPECT_EQ(result.value.clipLocalFrame, 4);
    const auto pts = result.value.source->pts();
    auto savedPlan = result.value.plan;
    executor.close();
    EXPECT_EQ(result.value.source->pts(), pts); ASSERT_TRUE(savedPlan); EXPECT_EQ(savedPlan->clips.front().id, "picture");
    ASSERT_TRUE(executor.openSource(HEISENBERG_TEST_MEDIA, {}));
    auto source = executor.videoFrame(6, {7}); ASSERT_TRUE(source); ASSERT_TRUE(source.value.source);
    EXPECT_EQ(result.value.source->pts(), source.value.source->pts());
    EXPECT_TRUE(source.value.sourceMode);
}
#endif
