#include <Compositing/CompositionExecutor.hpp>
#include <Preview/VideoEngine.hpp>
#include <MultiMedia/DemuxContext.hpp>
#include <MultiMedia/DecoderContext.hpp>
#include <Common/Packet.hpp>
#include <Common/Stream.hpp>
#include <Common/MediaMath.hpp>
#include <Utiles/Logger.hpp>
#include <spdlog/sinks/base_sink.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
}
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <regex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

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
namespace {
CompositionPlan sharedMediaPlan() {
    CompositionPlan plan; plan.frameCount = 24;
    CompositionClip clip;
    clip.id = "original"; clip.mediaRef = "shared-asset"; clip.sourcePath = HEISENBERG_TEST_MEDIA;
    clip.durationFrames = plan.frameCount;
    plan.clips = {clip};
    return plan;
}
CompositionPlan originalSoundReference(const CompositionPlan& plan) {
    auto reference = plan;
    reference.audioClips = reference.clips;
    reference.clips.clear();
    reference.audioMuted = reference.muted;
    return reference;
}
std::filesystem::path unsupportedDecoderFixture(bool unsupportedVideo = false) {
    static std::atomic<unsigned> sequence{0};
    const auto path = std::filesystem::temp_directory_path() / ("heisenberg-unsupported-decoder-"
        + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())) + "-"
        + std::to_string(sequence.fetch_add(1)) + ".avi");
    struct Resources {
        AVFormatContext* source = nullptr;
        AVFormatContext* output = nullptr;
        AVPacket* packet = nullptr;
        AVBSFContext* filter = nullptr;
        std::filesystem::path path;
        bool complete = false;
        ~Resources() {
            av_packet_free(&packet);
            av_bsf_free(&filter);
            avformat_close_input(&source);
            if (output) {
                if (output->pb) avio_closep(&output->pb);
                avformat_free_context(output);
            }
            if (!complete) { std::error_code error; std::filesystem::remove(path, error); }
        }
    } resources;
    resources.path = path;
    if (avformat_open_input(&resources.source, HEISENBERG_TEST_MEDIA, nullptr, nullptr) < 0
        || avformat_find_stream_info(resources.source, nullptr) < 0) return {};
    const int inputIndex = av_find_best_stream(resources.source, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (inputIndex < 0) return {};
    auto* inputVideo = resources.source->streams[inputIndex];
    if (avformat_alloc_output_context2(&resources.output, nullptr, "avi", path.string().c_str()) < 0
        || !resources.output) return {};
    const auto* filter = av_bsf_get_by_name("h264_mp4toannexb");
    if (!filter || av_bsf_alloc(filter, &resources.filter) < 0
        || avcodec_parameters_copy(resources.filter->par_in, inputVideo->codecpar) < 0) return {};
    resources.filter->time_base_in = inputVideo->time_base;
    if (av_bsf_init(resources.filter) < 0) return {};
    auto* video = avformat_new_stream(resources.output, nullptr);
    auto* audio = avformat_new_stream(resources.output, nullptr);
    if (!video || !audio || avcodec_parameters_copy(video->codecpar, resources.filter->par_out) < 0) return {};
    video->codecpar->codec_tag = 0;
    video->time_base = inputVideo->time_base; video->avg_frame_rate = inputVideo->avg_frame_rate;
    audio->time_base = AVRational{1, 48000};
    audio->codecpar->codec_type = AVMEDIA_TYPE_AUDIO; audio->codecpar->codec_id = AV_CODEC_ID_PCM_S16LE;
    audio->codecpar->format = AV_SAMPLE_FMT_S16; audio->codecpar->sample_rate = 48000;
    av_channel_layout_default(&audio->codecpar->ch_layout, 1);
    audio->codecpar->bits_per_coded_sample = 16; audio->codecpar->bits_per_raw_sample = 16;
    audio->codecpar->block_align = 2; audio->codecpar->bit_rate = 48000 * 16;
    if (avio_open(&resources.output->pb, path.string().c_str(), AVIO_FLAG_WRITE) < 0
        || avformat_write_header(resources.output, nullptr) < 0) return {};
    resources.packet = av_packet_alloc();
    if (!resources.packet) return {};
    int64_t audioPosition = 0;
    int read = 0;
    while ((read = av_read_frame(resources.source, resources.packet)) >= 0) {
        auto* packet = resources.packet;
        if (packet->stream_index != inputIndex) { av_packet_unref(packet); continue; }
        if (av_bsf_send_packet(resources.filter, packet) < 0
            || av_bsf_receive_packet(resources.filter, packet) < 0) return {};
        av_packet_rescale_ts(packet, inputVideo->time_base, video->time_base);
        packet->stream_index = video->index; packet->pos = -1;
        if (av_interleaved_write_frame(resources.output, packet) < 0) return {};
        av_packet_unref(packet);
        if (av_new_packet(packet, 4000) < 0) return {};
        // Nonzero PCM makes the inverse (optional video failure) test meaningful.
        std::fill_n(packet->data, packet->size, uint8_t{20});
        packet->stream_index = audio->index; packet->pts = packet->dts = audioPosition;
        packet->duration = 2000; packet->flags = AV_PKT_FLAG_KEY;
        av_packet_rescale_ts(packet, AVRational{1, 48000}, audio->time_base);
        if (av_interleaved_write_frame(resources.output, packet) < 0) return {};
        av_packet_unref(packet); audioPosition += 2000;
    }
    if (read != AVERROR_EOF || audioPosition == 0 || av_write_trailer(resources.output) < 0
        || avio_closep(&resources.output->pb) < 0) return {};
    std::vector<uint8_t> bytes;
    {
        std::ifstream input(path, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        if (input.bad()) return {};
    }
    bool changed = false;
    for (size_t offset = 0; offset + 28 <= bytes.size(); ++offset) {
        if (std::memcmp(bytes.data() + offset, "strf", 4) != 0) continue;
        if (unsupportedVideo && bytes[offset + 8] >= 40 && bytes[offset + 9] == 0
            && bytes[offset + 10] == 0 && bytes[offset + 11] == 0) {
            std::fill_n(bytes.data() + offset + 24, 4, uint8_t{'Z'});
            changed = true; break;
        }
        if (!unsupportedVideo && bytes[offset + 8] == 1 && bytes[offset + 9] == 0) {
            bytes[offset + 8] = 0xff; bytes[offset + 9] = 0xff;
            changed = true; break;
        }
    }
    if (!changed) return {};
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        output.close();
        if (!output) return {};
    }
    resources.complete = true;
    return path;
}
// Compare the stored float bytes, not a tolerance (or even signed-zero equality).
void bitIdenticalRange(const AudioFrame& expected, const AudioFrame& actual) {
    ASSERT_EQ(expected.spec(), actual.spec());
    ASSERT_GE(actual.position(), expected.position());
    ASSERT_LE(actual.position() + actual.samples(), expected.position() + expected.samples());
    const auto offset = static_cast<int>(actual.position() - expected.position());
    for (int channel = 0; channel < actual.spec().channels; ++channel)
        EXPECT_EQ(std::memcmp(expected.channelRo(channel) + offset, actual.channelRo(channel),
            static_cast<size_t>(actual.samples()) * sizeof(float)), 0) << "channel " << channel;
}
}

TEST(CompositionExecutorTest, SequentialSharedVideoAndOriginalSoundMatchAudioOnlyInPlanAndSourceModes) {
    const auto plan = sharedMediaPlan();
    CompositionExecutor reference;
    ASSERT_TRUE(reference.open(originalSoundReference(plan), {}));
    auto baseline = reference.audioSamples(0, reference.info().audioSampleCount);
    ASSERT_TRUE(baseline); ASSERT_TRUE(baseline.value); ASSERT_GT(energy(*baseline.value), 0.01);
    for (const bool sourceMode : {false, true}) {
        SCOPED_TRACE(sourceMode ? "openSource" : "open(plan)");
        CompositionExecutor shared;
        const auto opened = sourceMode ? shared.openSource(HEISENBERG_TEST_MEDIA, {}) : shared.open(plan, {});
        ASSERT_TRUE(opened) << opened.error;
        ASSERT_GE(opened.value.frameCount, 24);
        ASSERT_EQ(opened.value.settings.timeBase(), plan.settings.timeBase());
        for (int64_t frame = 0; frame < 24; ++frame) {
            SCOPED_TRACE(frame);
            auto video = shared.videoFrame(frame);
            ASSERT_TRUE(video) << video.error; ASSERT_TRUE(video.value.source);
            EXPECT_FALSE(video.value.blank);
            const auto begin = plan.settings.audioSamplePosition(frame);
            const auto end = plan.settings.audioSamplePosition(frame + 1);
            auto audio = shared.audioSamples(begin, end);
            ASSERT_TRUE(audio) << audio.error; ASSERT_TRUE(audio.value);
            ASSERT_EQ(audio.value->position(), begin); ASSERT_EQ(audio.value->samples(), end - begin);
            bitIdenticalRange(*baseline.value, *audio.value);
        }
    }
}

TEST(CompositionExecutorTest, SharedOriginalSoundIsBitIdenticalAcrossAudioFirstAndInterleavedFrameJumps) {
    auto plan = sharedMediaPlan();
    auto& clip = plan.clips.front();
    clip.startFrame = 2; clip.durationFrames = 22; clip.trimStartFrame = 1;
    clip.speed = 0.85; clip.volume = 0.6; clip.fadeInFrames = 2; clip.fadeOutFrames = 4;
    CompositionExecutor reference;
    ASSERT_TRUE(reference.open(originalSoundReference(plan), {}));
    auto baseline = reference.audioSamples(0, reference.info().audioSampleCount);
    ASSERT_TRUE(baseline); ASSERT_TRUE(baseline.value); ASSERT_GT(energy(*baseline.value), 0.01);

    for (const bool audioFirst : {true, false}) {
        SCOPED_TRACE(audioFirst ? "audio first" : "video first");
        CompositionExecutor shared, pictures;
        ASSERT_TRUE(shared.open(plan, {})); ASSERT_TRUE(pictures.open(plan, {}));
        const std::array<int64_t, 6> frames{2, 17, 4, 23, 9, 2};
        const std::array<std::pair<int64_t, int64_t>, 6> ranges{{
            {4000, 8001}, {24000, 37001}, {4000, 9001}, {38000, 44000}, {12000, 20001}, {1, 4217}}};
        for (size_t index = 0; index < frames.size(); ++index) {
            SCOPED_TRACE(index);
            // Most seeks stay in one generation; the last also exercises reset.
            const CompositionExecutor::Context context(index + 1 == frames.size() ? 99 : 7);
            const auto range = ranges[index];
            if (audioFirst) {
                auto actual = shared.audioSamples(range.first, range.second, context);
                ASSERT_TRUE(actual) << actual.error; ASSERT_TRUE(actual.value);
                ASSERT_EQ(actual.value->position(), range.first);
                ASSERT_EQ(actual.value->samples(), range.second - range.first);
                bitIdenticalRange(*baseline.value, *actual.value);
            }
            auto expectedVideo = pictures.videoFrame(frames[index], context);
            auto actualVideo = shared.videoFrame(frames[index], context);
            ASSERT_TRUE(expectedVideo) << expectedVideo.error; ASSERT_TRUE(expectedVideo.value.source);
            ASSERT_TRUE(actualVideo) << actualVideo.error; ASSERT_TRUE(actualVideo.value.source);
            EXPECT_EQ(actualVideo.value.source->pts(), expectedVideo.value.source->pts());
            if (!audioFirst) {
                auto actual = shared.audioSamples(range.first, range.second, context);
                ASSERT_TRUE(actual) << actual.error; ASSERT_TRUE(actual.value);
                ASSERT_EQ(actual.value->position(), range.first);
                ASSERT_EQ(actual.value->samples(), range.second - range.first);
                bitIdenticalRange(*baseline.value, *actual.value);
            }
        }
    }
}

TEST(CompositionExecutorTest, HiddenSharedVideoKeepsBitIdenticalOriginalSound) {
    auto plan = sharedMediaPlan(); plan.hidden = true;
    CompositionExecutor reference, hidden;
    ASSERT_TRUE(reference.open(originalSoundReference(plan), {})); ASSERT_TRUE(hidden.open(plan, {}));
    auto baseline = reference.audioSamples(0, reference.info().audioSampleCount);
    ASSERT_TRUE(baseline); ASSERT_TRUE(baseline.value); ASSERT_GT(energy(*baseline.value), 0.01);
    for (const int64_t frame : {int64_t{0}, int64_t{16}, int64_t{3}}) {
        auto video = hidden.videoFrame(frame);
        ASSERT_TRUE(video) << video.error; EXPECT_TRUE(video.value.blank); EXPECT_FALSE(video.value.source);
        const auto begin = plan.settings.audioSamplePosition(frame);
        auto actual = hidden.audioSamples(begin, begin + 4000);
        ASSERT_TRUE(actual) << actual.error; ASSERT_TRUE(actual.value);
        ASSERT_EQ(actual.value->samples(), 4000);
        bitIdenticalRange(*baseline.value, *actual.value);
    }
}

TEST(CompositionExecutorTest, SamePathIndependentAudioKeepsItsOwnTrimSpeedAndContributionOffset) {
    auto plan = sharedMediaPlan();
    auto& original = plan.clips.front();
    original.speed = 0.75; original.volume = 0.35; original.fadeInFrames = 2; original.fadeOutFrames = 3;
    auto independent = original;
    independent.id = "independent"; independent.startFrame = 5; independent.durationFrames = 17;
    independent.trimStartFrame = 4; independent.speed = 0.6; independent.volume = 0.4;
    independent.fadeInFrames = 3; independent.fadeOutFrames = 2;
    plan.audioClips = {independent};
    // Separate, nonoverlapping-lane references: both use only audioClips, so no
    // original-video decoder/session is involved in computing the expected mix.
    auto independentPlan = plan; independentPlan.clips.clear();
    CompositionExecutor originalReference, independentReference, shared;
    ASSERT_TRUE(originalReference.open(originalSoundReference(plan), {}));
    ASSERT_TRUE(independentReference.open(independentPlan, {})); ASSERT_TRUE(shared.open(plan, {}));
    const auto total = shared.info().audioSampleCount;
    auto expected = originalReference.audioSamples(0, total);
    auto extra = independentReference.audioSamples(0, total);
    ASSERT_TRUE(expected); ASSERT_TRUE(expected.value); ASSERT_TRUE(extra); ASSERT_TRUE(extra.value);
    ASSERT_GT(energy(*expected.value), 0.01); ASSERT_GT(energy(*extra.value), 0.005);
    for (int channel = 0; channel < expected.value->spec().channels; ++channel)
        for (int sample = 0; sample < expected.value->samples(); ++sample)
            expected.value->sampleRw(channel, sample) = std::clamp(expected.value->sampleRo(channel, sample)
                + extra.value->sampleRo(channel, sample), -1.0f, 1.0f);
    const std::array<int64_t, 5> frames{3, 18, 7, 23, 0};
    for (const auto frame : frames) {
        auto video = shared.videoFrame(frame); ASSERT_TRUE(video) << video.error; ASSERT_TRUE(video.value.source);
        const auto begin = plan.settings.audioSamplePosition(frame);
        auto actual = shared.audioSamples(begin, std::min(total, begin + 5001));
        ASSERT_TRUE(actual) << actual.error; ASSERT_TRUE(actual.value);
        ASSERT_EQ(actual.value->samples(), std::min(total, begin + 5001) - begin);
        bitIdenticalRange(*expected.value, *actual.value);
    }
}

TEST(CompositionExecutorTest, AudioOnlyAdvanceReleasesSharedVideoImageAndCanRecoverVideo) {
    auto plan = sharedMediaPlan();
    CompositionExecutor executor; ASSERT_TRUE(executor.open(plan, {}));
    std::weak_ptr<VideoFrame> image;
    {
        auto video = executor.videoFrame(0); ASSERT_TRUE(video); ASSERT_TRUE(video.value.source);
        image = video.value.source;
    }
    ASSERT_FALSE(image.expired()); // The selected image, not a returned result, owns this reference.
    auto audio = executor.audioSamples(24000, 30000); ASSERT_TRUE(audio) << audio.error;
    EXPECT_TRUE(image.expired());
    auto recovered = executor.videoFrame(2); ASSERT_TRUE(recovered) << recovered.error;
    ASSERT_TRUE(recovered.value.source);
    CompositionExecutor reference; ASSERT_TRUE(reference.open(plan, {}));
    auto expected = reference.videoFrame(2); ASSERT_TRUE(expected); ASSERT_TRUE(expected.value.source);
    EXPECT_EQ(recovered.value.source->pts(), expected.value.source->pts());
}

TEST(CompositionExecutorTest, SharedVideoRetainsOnlyChosenImageAndReleasesItInVideoGap) {
    auto plan = sharedMediaPlan();
    plan.clips.front().durationFrames = 20;
    CompositionExecutor executor; ASSERT_TRUE(executor.open(plan, {}));
    std::weak_ptr<VideoFrame> previous;
    for (const int64_t frame : {int64_t{0}, int64_t{1}, int64_t{7}, int64_t{12}, int64_t{18}}) {
        std::weak_ptr<VideoFrame> chosen;
        {
            auto video = executor.videoFrame(frame); ASSERT_TRUE(video) << video.error;
            ASSERT_TRUE(video.value.source); chosen = video.value.source;
        }
        EXPECT_TRUE(previous.expired()) << "old image retained at frame " << frame;
        EXPECT_FALSE(chosen.expired()) << "chosen image was not retained at frame " << frame;
        previous = chosen;
    }
    auto gap = executor.videoFrame(22); ASSERT_TRUE(gap); EXPECT_TRUE(gap.value.blank);
    EXPECT_FALSE(gap.value.source); EXPECT_TRUE(previous.expired());
}

TEST(CompositionExecutorTest, UnsupportedOptionalAudioDoesNotKillUsableVideo) {
    const auto path = unsupportedDecoderFixture();
    ASSERT_FALSE(path.empty());
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove(path, error); }
    } cleanup{path};

    demuxer::DemuxContext demux;
    ASSERT_GE(demux.open(path.string()), 0);
    const auto audio = std::find_if(demux.streams().begin(), demux.streams().end(),
        [](const Stream& stream) { return stream.isAudio(); });
    const auto video = std::find_if(demux.streams().begin(), demux.streams().end(),
        [](const Stream& stream) { return stream.isVideo(); });
    ASSERT_NE(audio, demux.streams().end()); ASSERT_NE(video, demux.streams().end());
    EXPECT_EQ(audio->codec.nativeCodecId, AV_CODEC_ID_NONE);
    decoder::DecoderContext rejectedAudio;
    EXPECT_LT(rejectedAudio.open(*audio), 0);

    CompositionPlan plan; plan.frameCount = 24;
    CompositionClip clip; clip.id = "usable-video"; clip.sourcePath = path.string(); clip.durationFrames = 24;
    plan.clips = {clip};
    CompositionExecutor executor; ASSERT_TRUE(executor.open(plan, {}));
    auto picture = executor.videoFrame(0);
    ASSERT_TRUE(picture) << picture.error; ASSERT_TRUE(picture.value.source);
    EXPECT_EQ(picture.value.clipId, "usable-video");
    auto silence = executor.audioSamples(0, 2000);
    ASSERT_TRUE(silence) << silence.error; ASSERT_TRUE(silence.value);
    EXPECT_EQ(silence.value->samples(), 2000); EXPECT_EQ(energy(*silence.value), 0.0);
    auto stillUsable = executor.videoFrame(0);
    ASSERT_TRUE(stillUsable) << stillUsable.error; ASSERT_TRUE(stillUsable.value.source);

    CompositionExecutor source;
    auto opened = source.openSource(path.string(), {});
    ASSERT_TRUE(opened) << opened.error; EXPECT_TRUE(opened.value.hasVideo);
    EXPECT_FALSE(opened.value.hasAudio);
    auto sourcePicture = source.videoFrame(0);
    ASSERT_TRUE(sourcePicture) << sourcePicture.error; ASSERT_TRUE(sourcePicture.value.source);
    auto sourceSilence = source.audioSamples(0, 1000);
    ASSERT_TRUE(sourceSilence) << sourceSilence.error; ASSERT_TRUE(sourceSilence.value);
    EXPECT_EQ(energy(*sourceSilence.value), 0.0);

    // The same failure is not optional for an independent audio-only lane.
    CompositionExecutor independent;
    ASSERT_TRUE(independent.open(originalSoundReference(plan), {}));
    auto failed = independent.audioSamples(0, 1000);
    EXPECT_EQ(failed.status, CompositionStatus::Failure);
    EXPECT_FALSE(failed.value); EXPECT_FALSE(failed.error.empty());

    plan.audioClips = plan.clips;
    plan.audioClips.front().id = "unsupported-independent-audio";
    CompositionExecutor mixed;
    ASSERT_TRUE(mixed.open(plan, {}));
    auto beforeFailure = mixed.videoFrame(0);
    ASSERT_TRUE(beforeFailure) << beforeFailure.error; ASSERT_TRUE(beforeFailure.value.source);
    EXPECT_EQ(mixed.audioSamples(0, 2000).status, CompositionStatus::Failure);
    auto afterFailure = mixed.videoFrame(1);
    ASSERT_TRUE(afterFailure) << afterFailure.error; ASSERT_TRUE(afterFailure.value.source);
}

TEST(CompositionExecutorTest, UnsupportedVideoDecoderStillAllowsOriginalAndIndependentAudio) {
    const auto path = unsupportedDecoderFixture(true);
    ASSERT_FALSE(path.empty());
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove(path, error); }
    } cleanup{path};
    demuxer::DemuxContext demux; ASSERT_GE(demux.open(path.string()), 0);
    const auto video = std::find_if(demux.streams().begin(), demux.streams().end(),
        [](const Stream& stream) { return stream.isVideo(); });
    const auto audio = std::find_if(demux.streams().begin(), demux.streams().end(),
        [](const Stream& stream) { return stream.isAudio(); });
    ASSERT_NE(video, demux.streams().end()); ASSERT_NE(audio, demux.streams().end());
    decoder::DecoderContext rejectedVideo, usableAudio;
    ASSERT_LT(rejectedVideo.open(*video), 0); ASSERT_GE(usableAudio.open(*audio), 0);
    auto plan = sharedMediaPlan(); plan.clips.front().sourcePath = path.string();
    CompositionExecutor reference; ASSERT_TRUE(reference.open(originalSoundReference(plan), {}));
    auto baseline = reference.audioSamples(0, 4000);
    ASSERT_TRUE(baseline) << baseline.error; ASSERT_TRUE(baseline.value);
    ASSERT_GT(energy(*baseline.value), 0.01);
    for (const bool sourceMode : {false, true}) {
        SCOPED_TRACE(sourceMode ? "openSource" : "open(plan)");
        CompositionExecutor executor;
        const auto opened = sourceMode ? executor.openSource(path.string(), {}) : executor.open(plan, {});
        ASSERT_TRUE(opened) << opened.error;
        auto blank = executor.videoFrame(0);
        ASSERT_TRUE(blank) << blank.error; EXPECT_TRUE(blank.value.blank); EXPECT_FALSE(blank.value.source);
        auto actual = executor.audioSamples(0, 4000);
        ASSERT_TRUE(actual) << actual.error; ASSERT_TRUE(actual.value);
        ASSERT_EQ(actual.value->samples(), 4000);
        bitIdenticalRange(*baseline.value, *actual.value);
    }
}

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

#ifdef HEISENBERG_TEST_EXECUTOR_LOOKAHEAD
namespace {
CompositionPlan executorLookaheadPlan() {
    CompositionPlan plan;
    plan.settings = TimelineSettings::fromTimelineSettings(160, 90, 30);
    plan.frameCount = 300;
    CompositionClip clip;
    clip.id = "lookahead";
    clip.mediaRef = "executor-lookahead";
    clip.sourcePath = HEISENBERG_TEST_EXECUTOR_LOOKAHEAD;
    clip.durationFrames = plan.frameCount;
    plan.clips = {clip};
    return plan;
}

CompositionPlan executorLookaheadAudioPlan(const CompositionPlan& source) {
    auto plan = source;
    plan.audioClips = plan.clips;
    plan.clips.clear();
    plan.audioMuted = plan.muted;
    return plan;
}

class ExecutorDiagnosticSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    std::vector<std::string> messages() {
        std::lock_guard lock(mutex_);
        return messages_;
    }
private:
    void sink_it_(const spdlog::details::log_msg& message) override {
        if (message.level != spdlog::level::info) return;
        const std::string text(message.payload.data(), message.payload.size());
        if (text.starts_with("[PlaybackDiag] session ")) messages_.push_back(text);
    }
    void flush_() override {}
    std::vector<std::string> messages_;
};

class CaptureExecutorDiagnostics {
public:
    CaptureExecutorDiagnostics() {
        logger_ = Logger::ModuleLogger("Framework/Compositing/CompositionExecutor.cpp");
        sinks_ = logger_->sinks();
        level_ = logger_->level();
        logger_->sinks().push_back(sink);
        logger_->set_level(spdlog::level::info);
    }
    ~CaptureExecutorDiagnostics() {
        logger_->sinks() = std::move(sinks_);
        logger_->set_level(level_);
    }
    std::shared_ptr<ExecutorDiagnosticSink> sink = std::make_shared<ExecutorDiagnosticSink>();
private:
    spdlog::logger* logger_ = nullptr;
    std::vector<spdlog::sink_ptr> sinks_;
    spdlog::level::level_enum level_ = spdlog::level::info;
};

struct ExecutorDiagnosticTotals {
    uint64_t reopens = 0;
    uint64_t videoFrames = 0;
    bool found = false;
};

ExecutorDiagnosticTotals diagnosticTotals(const std::vector<std::string>& messages,
                                          const std::string& path) {
    ExecutorDiagnosticTotals totals;
    const std::regex pathPattern("path='([^']*)'");
    const std::regex reopenPattern("reopens=([0-9]+)");
    const std::regex videoPattern("video_frames=([0-9]+)");
    for (const auto& message : messages) {
        std::smatch match;
        if (!std::regex_search(message, match, pathPattern) || match[1].str() != path) continue;
        totals.found = true;
        if (std::regex_search(message, match, reopenPattern))
            totals.reopens += std::stoull(match[1].str());
        if (std::regex_search(message, match, videoPattern))
            totals.videoFrames += std::stoull(match[1].str());
    }
    return totals;
}
}

TEST(CompositionExecutorTest, MappedThirtyFpsSequentialPlaybackDoesNotReplayOrigin) {
    const auto plan = executorLookaheadPlan();
    CompositionExecutor reference;
    ASSERT_TRUE(reference.open(executorLookaheadAudioPlan(plan), {}));
    auto baseline = reference.audioSamples(0, reference.info().audioSampleCount);
    ASSERT_TRUE(baseline) << baseline.error;
    ASSERT_TRUE(baseline.value);
    ASSERT_EQ(baseline.value->samples(), 480000);

    CaptureExecutorDiagnostics logs;
    CompositionExecutor executor;
    const auto opened = executor.open(plan, {});
    ASSERT_TRUE(opened) << opened.error;
    ASSERT_EQ(opened.value.settings.frameRate(), Rational(30, 1));
    ASSERT_EQ(opened.value.settings.timeBase(), Rational(1, 30));
    ASSERT_EQ(opened.value.frameCount, 300);

    for (int64_t frame = 0; frame < plan.frameCount; ++frame) {
        SCOPED_TRACE(frame);
        auto picture = executor.videoFrame(frame);
        ASSERT_TRUE(picture) << picture.error;
        ASSERT_TRUE(picture.value.source);
        const auto expectedSourceFrame = (24 * frame) / 30;
        EXPECT_EQ(frameIndexFromTimestamp(*picture.value.source->native(), AVRational{24, 1}),
                  expectedSourceFrame);

        const auto begin = plan.settings.audioSamplePosition(frame);
        const auto end = plan.settings.audioSamplePosition(frame + 1);
        auto audio = executor.audioSamples(begin, end);
        ASSERT_TRUE(audio) << audio.error;
        ASSERT_TRUE(audio.value);
        ASSERT_EQ(audio.value->position(), begin);
        ASSERT_EQ(audio.value->samples(), end - begin);
        bitIdenticalRange(*baseline.value, *audio.value);

        if (frame == plan.frameCount - 2)
            std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    }

    const auto totals = diagnosticTotals(logs.sink->messages(),
                                         HEISENBERG_TEST_EXECUTOR_LOOKAHEAD);
    ASSERT_TRUE(totals.found);
    EXPECT_EQ(totals.reopens, 0u);
    EXPECT_LE(totals.videoFrames, 24u * 10u + 4u);
}

TEST(CompositionExecutorTest, MappedPlaybackBackwardRequestReopensAndPreservesPtsAndPcm) {
    const auto plan = executorLookaheadPlan();
    CompositionExecutor reference;
    ASSERT_TRUE(reference.open(executorLookaheadAudioPlan(plan), {}));
    auto baseline = reference.audioSamples(0, reference.info().audioSampleCount);
    ASSERT_TRUE(baseline) << baseline.error;
    ASSERT_TRUE(baseline.value);

    CaptureExecutorDiagnostics logs;
    CompositionExecutor executor;
    ASSERT_TRUE(executor.open(plan, {}));
    auto forward = executor.videoFrame(240);
    ASSERT_TRUE(forward) << forward.error;
    ASSERT_TRUE(forward.value.source);
    EXPECT_EQ(frameIndexFromTimestamp(*forward.value.source->native(), AVRational{24, 1}), 192);

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    auto backward = executor.videoFrame(30);
    ASSERT_TRUE(backward) << backward.error;
    ASSERT_TRUE(backward.value.source);
    EXPECT_EQ(frameIndexFromTimestamp(*backward.value.source->native(), AVRational{24, 1}), 24);

    const auto begin = plan.settings.audioSamplePosition(30);
    const auto end = plan.settings.audioSamplePosition(31);
    auto audio = executor.audioSamples(begin, end);
    ASSERT_TRUE(audio) << audio.error;
    ASSERT_TRUE(audio.value);
    bitIdenticalRange(*baseline.value, *audio.value);

    const auto totals = diagnosticTotals(logs.sink->messages(),
                                         HEISENBERG_TEST_EXECUTOR_LOOKAHEAD);
    ASSERT_TRUE(totals.found);
    EXPECT_GT(totals.reopens, 0u);
}
#endif
#endif
