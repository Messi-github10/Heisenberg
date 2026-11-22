#include <Compositing/CompositionBuilder.hpp>
#include <Compositing/CompositionExecutor.hpp>
#include <Models/BlendMode.hpp>
#include <MultiMedia/Video/Renderer/Color/ColorPipeline.hpp>
#include <Export/ExportEngine.hpp>
#include <Project/VideoProject.hpp>
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <functional>
#include <limits>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}
using namespace heisenberg;
namespace {
namespace fs = std::filesystem;
std::string utf8(const fs::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}
struct OutputFolder {
    fs::path root = fs::temp_directory_path() / ("heisenberg-export-" + makeUuid());
    OutputFolder() { fs::create_directories(root); }
    ~OutputFolder() { std::error_code ignored; fs::remove_all(root, ignored); }
    fs::path output() const {
        return root / "成片.mp4";
    }
    bool noStaging() const {
        for (const auto& entry : fs::directory_iterator(root))
            if (entry.path().extension() == ".partial") return false;
        return true;
    }
};
ProjectSnapshot snapshot(int frames = 24, bool picture = true, bool music = true, int width = 160) {
    VideoProject project;
    ProjectFile document;
    Timeline timeline;
    timeline.id = "timeline"; timeline.width = width; timeline.height = 120; timeline.fps = 24;
    MediaManifest manifest;
    MediaManifestEntry video;
    video.id = "v"; video.type = ClipType::Video;
    video.source.path = HEISENBERG_PLAYLIST_CLIP_A; video.duration = 1.0; video.hasAudio = true;
    manifest.add(video);
    MediaManifestEntry audio;
    audio.id = "a"; audio.type = ClipType::Audio;
    audio.source.path = HEISENBERG_TEST_LONG_AUDIO; audio.duration = 3.0; audio.hasAudio = true;
    manifest.add(audio);
    if (picture) {
        Track track; track.id = "video";
        Clip clip; clip.id = "picture"; clip.mediaRef = "v"; clip.durationFrames = std::min(frames, 24);
        track.clips.push_back(clip); timeline.tracks.push_back(track);
    }
    if (music) {
        Track track; track.id = "audio"; track.type = ClipType::Audio;
        Clip clip; clip.id = "music"; clip.mediaRef = "a";
        clip.mediaType = clip.sourceClipType = ClipType::Audio; clip.durationFrames = frames;
        track.clips.push_back(clip); timeline.tracks.push_back(track);
    }
    document.timelines.push_back(timeline); document.activeTimelineId = timeline.id;
    document.colorSettings.inputColorSpaces["v"] = "Rec.1886 Rec.709 - Display";
    std::string error;
    EXPECT_TRUE(project.editor().replaceDocument(document, manifest, "", &error)) << error;
    return project.snapshot();
}
struct MediaReadback {
    int pictures = 0;
    int64_t samples = 0;
    double audioEnd = 0;
    std::vector<float> audio;
    std::vector<double> brightness;
    std::vector<std::vector<double>> grayPatches;
    std::vector<std::vector<uint8_t>> luma;
    std::vector<double> videoTimes;
};
MediaReadback readback(const fs::path& path, AVCodecID expectedVideo, int expectedFrames) {
    AVFormatContext* input = nullptr;
    EXPECT_GE(avformat_open_input(&input, utf8(path).c_str(), nullptr, nullptr), 0);
    if (!input) return {};
    std::unique_ptr<AVFormatContext, void(*)(AVFormatContext*)> format(input,
        [](AVFormatContext* context) { avformat_close_input(&context); });
    EXPECT_GE(avformat_find_stream_info(input, nullptr), 0);
    const int vi = av_find_best_stream(input, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    const int ai = av_find_best_stream(input, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    EXPECT_GE(vi, 0); EXPECT_GE(ai, 0);
    if (vi < 0 || ai < 0) return {};
    EXPECT_EQ(input->streams[vi]->codecpar->codec_id, expectedVideo);
    EXPECT_EQ(input->streams[ai]->codecpar->codec_id, AV_CODEC_ID_AAC);
    EXPECT_EQ(input->streams[vi]->codecpar->width, 160);
    EXPECT_EQ(input->streams[vi]->codecpar->height, 120);
    EXPECT_EQ(input->streams[vi]->codecpar->color_space, AVCOL_SPC_BT709);
    EXPECT_EQ(input->streams[vi]->codecpar->color_range, AVCOL_RANGE_MPEG);
    EXPECT_EQ(input->streams[vi]->nb_frames, expectedFrames);
    AVCodecContext* decoders[2]{};
    for (int i = 0; i < 2; ++i) {
        auto* stream = input->streams[i ? ai : vi];
        decoders[i] = avcodec_alloc_context3(avcodec_find_decoder(stream->codecpar->codec_id));
        EXPECT_NE(decoders[i], nullptr);
        EXPECT_GE(avcodec_parameters_to_context(decoders[i], stream->codecpar), 0);
        EXPECT_GE(avcodec_open2(decoders[i], nullptr, nullptr), 0);
    }
    MediaReadback result;
    AVFrame* frame = av_frame_alloc();
    auto pull = [&](int index) {
        int status = 0;
        while ((status = avcodec_receive_frame(decoders[index], frame)) == 0) {
            if (!index) {
                ++result.pictures;
                EXPECT_EQ(frame->format, AV_PIX_FMT_YUV420P);
                std::vector<uint8_t> luma(frame->width * frame->height);
                for (int y = 0; y < frame->height; ++y)
                    std::copy_n(frame->data[0] + y * frame->linesize[0], frame->width,
                                luma.data() + y * frame->width);
                result.luma.push_back(std::move(luma));
                result.videoTimes.push_back(frame->best_effort_timestamp == AV_NOPTS_VALUE ? -1.0
                    : frame->best_effort_timestamp * av_q2d(input->streams[vi]->time_base));
                double value = 0;
                for (int y = 0; y < frame->height; ++y) for (int x = 0; x < frame->width; ++x)
                    value += frame->data[0][y * frame->linesize[0] + x];
                result.brightness.push_back(value / (frame->width * frame->height));
                std::vector<double> patches;
                for (int patch = 0; patch < 8; ++patch) {
                    double total = 0;
                    int count = 0;
                    for (int y = 16; y < frame->height - 16; ++y)
                        for (int x = patch * 20 + 5; x < patch * 20 + 15 && x < frame->width; ++x) {
                            total += frame->data[0][y * frame->linesize[0] + x];
                            ++count;
                        }
                    patches.push_back(count ? total / count : 0);
                }
                result.grayPatches.push_back(std::move(patches));
            } else {
                EXPECT_EQ(frame->sample_rate, 48000);
                EXPECT_EQ(frame->format, AV_SAMPLE_FMT_FLTP);
                result.samples += frame->nb_samples;
                if (frame->pts != AV_NOPTS_VALUE)
                    result.audioEnd = std::max(result.audioEnd, frame->pts * av_q2d(input->streams[ai]->time_base)
                        + static_cast<double>(frame->nb_samples) / frame->sample_rate);
                if (frame->format == AV_SAMPLE_FMT_FLTP) {
                    const float* samples = reinterpret_cast<const float*>(frame->data[0]);
                    result.audio.insert(result.audio.end(), samples, samples + frame->nb_samples);
                }
            }
            av_frame_unref(frame);
        }
        EXPECT_TRUE(status == AVERROR(EAGAIN) || status == AVERROR_EOF) << status;
    };
    AVPacket* packet = av_packet_alloc();
    int64_t lastDts[2]{AV_NOPTS_VALUE, AV_NOPTS_VALUE};
    int status;
    while ((status = av_read_frame(input, packet)) >= 0) {
        const int index = packet->stream_index == vi ? 0 : packet->stream_index == ai ? 1 : -1;
        if (index >= 0) {
            if (packet->dts != AV_NOPTS_VALUE && lastDts[index] != AV_NOPTS_VALUE)
                EXPECT_GT(packet->dts, lastDts[index]);
            lastDts[index] = packet->dts;
            EXPECT_GE(avcodec_send_packet(decoders[index], packet), 0);
            pull(index);
        }
        av_packet_unref(packet);
    }
    EXPECT_EQ(status, AVERROR_EOF);
    for (int i = 0; i < 2; ++i) { EXPECT_GE(avcodec_send_packet(decoders[i], nullptr), 0); pull(i); avcodec_free_context(&decoders[i]); }
    av_packet_free(&packet); av_frame_free(&frame);
    return result;
}
double tone(const std::vector<float>& pcm, double hz, double begin, double end) {
    const auto first = static_cast<size_t>(begin * 48000), last = std::min(pcm.size(), static_cast<size_t>(end * 48000));
    if (last <= first) return 0;
    double re = 0, im = 0;
    for (size_t s = first; s < last; ++s) {
        const double phase = 6.283185307179586 * hz * s / 48000.0;
        re += pcm[s] * std::cos(phase); im += pcm[s] * std::sin(phase);
    }
    return 2.0 * std::hypot(re, im) / (last - first);
}
double geometryPatch(const std::vector<uint8_t>& pixels, int x, int y) {
    double total = 0;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) total += pixels[(y + dy) * 160 + x + dx];
    return total / 9.0;
}

std::string contents(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
}

TEST(ExportEngineTest, H264AndH265ExportAndFullyDecode) {
    for (auto codec : {ExportVideoCodec::H264, ExportVideoCodec::H265}) {
        OutputFolder folder;
        ExportEngine engine;
        ExportRequest request;
        request.snapshot = snapshot(); request.outputPath = utf8(folder.output());
        request.videoCodec = codec; request.crf = codec == ExportVideoCodec::H264 ? 20 : 24;
        std::atomic<int> terminal{0};
        engine.onFinished = [&](const ExportResult&) { ++terminal; };
        ASSERT_TRUE(engine.start(request)); engine.wait();
        const auto result = engine.result();
        ASSERT_TRUE(result.succeeded()) << result.stage << ": " << result.error;
        EXPECT_EQ(terminal.load(), 1); EXPECT_DOUBLE_EQ(engine.progress().fraction, 1.0);
        const auto decoded = readback(folder.output(), codec == ExportVideoCodec::H264 ? AV_CODEC_ID_H264 : AV_CODEC_ID_HEVC, 24);
        EXPECT_EQ(decoded.pictures, 24);
        EXPECT_GE(decoded.samples, 48000); EXPECT_LE(decoded.samples, 48000 + 1023);
        EXPECT_GT(tone(decoded.audio, 440, .15, .65), .03);
        EXPECT_GT(tone(decoded.audio, 880, .15, .65), .03);
        EXPECT_TRUE(folder.noStaging());
    }
}

TEST(ExportEngineTest, InsertThenRippleSaveReopenPreservesSourceMappingAndExportDuration) {
    OutputFolder folder;
    const auto initial = snapshot(24, true, false);
    auto document = initial.project();
    auto manifest = initial.manifest();
    auto& video = document.timelines.front().tracks.front();
    video.clips.front().durationFrames = 12;
    video.clips.front().trimStartFrame = 2;
    video.clips.front().trimEndFrame = 10;
    Clip removed;
    removed.id = "ripple-target"; removed.mediaRef = "v";
    removed.startFrame = 16; removed.durationFrames = 4;
    removed.trimStartFrame = 14; removed.trimEndFrame = 6;
    video.clips.push_back(removed);
    Clip tail;
    tail.id = "tail"; tail.mediaRef = "v";
    tail.startFrame = 24; tail.durationFrames = 8;
    tail.trimStartFrame = 16;
    video.clips.push_back(tail);
    MediaManifestEntry alternate;
    alternate.id = "alternate"; alternate.type = ClipType::Video;
    alternate.source.path = HEISENBERG_PLAYLIST_CLIP_B;
    alternate.duration = 1.0; alternate.hasAudio = true;
    ASSERT_TRUE(manifest.add(alternate));
    document.colorSettings.inputColorSpaces["alternate"] = "Rec.1886 Rec.709 - Display";

    VideoProject project;
    std::string error;
    ASSERT_TRUE(project.editor().replaceDocument(document, manifest, "", &error)) << error;
    const auto insert = project.editor().insertClip("alternate", 5, 6, 9, "video");
    ASSERT_TRUE(insert.changed()) << insert.message;
    ASSERT_FALSE(insert.affectedIds.empty());
    const std::string insertedId = insert.affectedIds.back();
    const auto* right = project.editor().activeTimeline()->trackById("video")->clipAt(11);
    ASSERT_NE(right, nullptr);
    const std::string rightId = right->id;
    ASSERT_NE(rightId, "picture");
    const auto ripple = project.editor().rippleDeleteClip("ripple-target");
    ASSERT_TRUE(ripple.changed()) << ripple.message;
    const auto edited = project.snapshot();

    // Independent expected half-open timeline -> source ranges, not computed
    // from the edited clips. Insertion splits picture; deletion preserves gaps.
    // [0,5)->v[2,7), [5,11)->alternate[9,15), [11,18)->v[7,14),
    // [18,26) is a gap, and [26,34)->v[16,24). Net length: 32 + 6 - 4.
    struct ExpectedRange {
        const char* mediaRef;
        int64_t start, duration, sourceStart, sourceEnd;
        std::string id, sourcePath;
    };
    const std::vector<ExpectedRange> expected{
        {"v", 0, 5, 2, 7, "picture", HEISENBERG_PLAYLIST_CLIP_A},
        {"alternate", 5, 6, 9, 15, insertedId, HEISENBERG_PLAYLIST_CLIP_B},
        {"v", 11, 7, 7, 14, rightId, HEISENBERG_PLAYLIST_CLIP_A},
        {"v", 26, 8, 16, 24, "tail", HEISENBERG_PLAYLIST_CLIP_A},
    };
    constexpr int expectedFrames = 34;
    constexpr double expectedSeconds = 34.0 / 24.0;
    const auto checkMapping = [&](const ProjectSnapshot& capture) {
        ASSERT_NE(capture.activeTimeline(), nullptr);
        EXPECT_EQ(capture.activeTimeline()->totalFrames(), expectedFrames);
        EXPECT_EQ(capture.activeTimeline()->clipById("ripple-target"), nullptr);
        CompositionPlan preview;
        ASSERT_TRUE(CompositionBuilder::build(capture, preview, &error)) << error;
        EXPECT_EQ(preview.frameCount, expectedFrames);
        EXPECT_NEAR(preview.settings.secondsFromFrame(preview.frameCount), expectedSeconds, 1e-9);
        ASSERT_EQ(preview.tracks.size(), 1u);
        ASSERT_EQ(preview.tracks.front().clips.size(), expected.size());
        EXPECT_EQ(preview.tracks.front().type, ClipType::Video); // Embedded video audio is also exported.
        for (size_t index = 0; index < expected.size(); ++index) {
            const auto& range = expected[index];
            SCOPED_TRACE(range.id);
            const auto* clip = capture.activeTimeline()->clipById(range.id);
            ASSERT_NE(clip, nullptr);
            EXPECT_EQ(clip->mediaRef, range.mediaRef);
            EXPECT_EQ(clip->startFrame, range.start);
            EXPECT_EQ(clip->durationFrames, range.duration);
            EXPECT_EQ(clip->trimStartFrame, range.sourceStart);
            EXPECT_EQ(clip->trimEndFrame, 24 - range.sourceEnd);
            EXPECT_DOUBLE_EQ(clip->speed, 1.0);
            const auto& planned = preview.tracks.front().clips[index];
            EXPECT_EQ(planned.id, range.id);
            EXPECT_EQ(planned.mediaRef, range.mediaRef);
            EXPECT_EQ(fs::u8path(planned.sourcePath).lexically_normal(),
                      fs::u8path(range.sourcePath).lexically_normal());
            EXPECT_EQ(planned.startFrame, range.start);
            EXPECT_EQ(planned.durationFrames, range.duration);
            EXPECT_EQ(planned.trimStartFrame, range.sourceStart);
            EXPECT_EQ(planned.trimEndFrame, 24 - range.sourceEnd);
            EXPECT_EQ(planned.visualSnapshot, *clip);
        }
        for (int64_t frame = 0; frame < expectedFrames; ++frame) {
            SCOPED_TRACE(frame);
            const auto range = std::find_if(expected.begin(), expected.end(), [&](const ExpectedRange& value) {
                return frame >= value.start && frame < value.start + value.duration;
            });
            const auto sample = preview.videoSampleAt(frame);
            if (range == expected.end()) {
                EXPECT_FALSE(sample); // The retained gap must not be packed away.
                continue;
            }
            ASSERT_TRUE(sample);
            const auto index = static_cast<size_t>(std::distance(expected.begin(), range));
            EXPECT_EQ(sample->clipIndex, index);
            EXPECT_EQ(sample->clipLocalFrame, frame - range->start);
            EXPECT_NEAR(sample->sourceSeconds, (range->sourceStart + frame - range->start) / 24.0, 1e-9);
        }
        EXPECT_FALSE(preview.videoSampleAt(expectedFrames));
    };
    ASSERT_NO_FATAL_FAILURE(checkMapping(edited));
    const auto package = folder.root / "edited.berg";
    ASSERT_TRUE(project.savePackage(utf8(package), &error)) << error;
    VideoProject reopened;
    ASSERT_TRUE(reopened.loadPackage(utf8(package), &error)) << error;
    const auto reopenedSnapshot = reopened.snapshot();
    EXPECT_EQ(reopenedSnapshot.project(), edited.project());
    EXPECT_EQ(reopenedSnapshot.manifest(), edited.manifest());
    ASSERT_NO_FATAL_FAILURE(checkMapping(reopenedSnapshot));

    ExportEngine engine;
    ExportRequest request;
    request.snapshot = reopenedSnapshot;
    request.outputPath = utf8(folder.output());
    request.videoCodec = ExportVideoCodec::H264;
    ASSERT_TRUE(engine.start(request));
    engine.wait();
    ASSERT_TRUE(engine.result().succeeded()) << engine.result().stage << ": " << engine.result().error;
    const auto decoded = readback(folder.output(), AV_CODEC_ID_H264, expectedFrames);
    ASSERT_EQ(decoded.pictures, expectedFrames);
    EXPECT_GE(decoded.samples, 68000); // 34 / 24 seconds at 48 kHz, with AAC padding only.
    EXPECT_LE(decoded.samples, 68000 + 1023);
    EXPECT_NEAR(decoded.audioEnd, expectedSeconds, 1024.0 / 48000.0);
    ASSERT_EQ(decoded.brightness.size(), static_cast<size_t>(expectedFrames));
    for (size_t frame = 18; frame < 26; ++frame) EXPECT_NEAR(decoded.brightness[frame], 16, 2.0);
    EXPECT_GT(decoded.brightness[26], 30.0); // Shifted tail returns after the preserved gap.

    AVFormatContext* input = nullptr;
    ASSERT_GE(avformat_open_input(&input, utf8(folder.output()).c_str(), nullptr, nullptr), 0);
    std::unique_ptr<AVFormatContext, void(*)(AVFormatContext*)> format(input,
        [](AVFormatContext* context) { avformat_close_input(&context); });
    ASSERT_GE(avformat_find_stream_info(input, nullptr), 0);
    const int videoIndex = av_find_best_stream(input, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    ASSERT_GE(videoIndex, 0);
    const auto* stream = input->streams[videoIndex];
    ASSERT_NE(stream->duration, AV_NOPTS_VALUE);
    EXPECT_NEAR(stream->duration * av_q2d(stream->time_base), expectedSeconds, 1e-6);
    EXPECT_TRUE(folder.noStaging());
}

TEST(ExportEngineTest, FinishedSdrGrayLevelsSurviveBothSoftwareEncodersWithoutGrade) {
    for (const auto codec : {ExportVideoCodec::H264, ExportVideoCodec::H265}) {
        VideoProject project;
        auto document = snapshot(6, true, false).project();
        document.colorSettings.inputColorSpaces.clear();
        MediaManifest manifest;
        MediaManifestEntry media;
        media.id = "v";
        media.type = ClipType::Video;
        media.source.path = HEISENBERG_TEST_SDR_GRAY;
        media.duration = 0.25;
        ASSERT_TRUE(manifest.add(media));
        std::string error;
        ASSERT_TRUE(project.editor().replaceDocument(document, manifest, "", &error)) << error;
        OutputFolder folder;
        ExportEngine engine;
        ExportRequest request;
        request.snapshot = project.snapshot();
        request.outputPath = utf8(folder.output());
        request.videoCodec = codec;
        request.crf = 0;
        ASSERT_TRUE(engine.start(request));
        engine.wait();
        ASSERT_TRUE(engine.result().succeeded()) << engine.result().stage << ": " << engine.result().error;
        const auto decoded = readback(folder.output(), codec == ExportVideoCodec::H264 ? AV_CODEC_ID_H264 : AV_CODEC_ID_HEVC, 6);
        ASSERT_EQ(decoded.grayPatches.size(), 6u);
        const int expected[]{16, 24, 40, 64, 96, 128, 192, 235};
        for (const auto& patches : decoded.grayPatches)
            for (int i = 0; i < 8; ++i) EXPECT_NEAR(patches[i], expected[i], 2.0) << "patch " << i;
    }
}

TEST(ExportEngineTest, EveryRegisteredBlendModePassesPreflightWithoutUnsupportedWarnings) {
    ASSERT_EQ(blendModes().size(), 17u);
    OutputFolder folder;
    const auto initial = snapshot(2, true, false);
    for (const auto& mode : blendModes()) {
        SCOPED_TRACE(std::string(mode.name));
        auto document = initial.project();
        document.timelines.front().tracks.front().clips.front().blendMode = std::string(mode.name);
        VideoProject project;
        std::string error;
        ASSERT_TRUE(project.editor().replaceDocument(document, initial.manifest(), "", &error)) << error;
        EXPECT_EQ(project.editor().compatibility().level, CompatibilityLevel::Supported);
        ExportRequest request; request.snapshot = project.snapshot();
        request.outputPath = utf8(folder.root / "absent" / "output.mp4");
        ExportEngine engine;
        ASSERT_TRUE(engine.start(request, &error)) << error; engine.wait();
        // The intentional path error proves appearance preflight accepted this
        // mode, without depending on GPU initialization or writing an output.
        EXPECT_EQ(engine.result().state, ExportState::Failed);
        EXPECT_EQ(engine.result().error, "Output directory does not exist");
        EXPECT_TRUE(engine.result().warnings.empty());
        EXPECT_FALSE(fs::exists(folder.root / "absent"));
        EXPECT_TRUE(folder.noStaging());
    }
}

TEST(ExportEngineTest, BlendModesFullyDecodeAgainstIndependentLinearReference) {
    namespace color = heisenberg::renderer::color;
    color::ColorPipeline pipeline;
    const auto toOutput = pipeline.displayProcessor(color::kWorkingSpace, color::kExportDisplay,
                                                    color::kColorimetricView);
    // Measure the encoded fixture through the software decoder: its stored Y
    // levels differ from the generator's nominal constants, and Cr is not
    // exactly neutral. Assuming neutral gray amplifies errors near black after
    // multiply/overlay and the display transfer. No renderer output is used.
    CompositionExecutor sourceDecoder;
    const auto opened = sourceDecoder.openSource(HEISENBERG_TEST_SDR_GRAY,
                                                 {false, CompositionErrorPolicy::Strict});
    ASSERT_TRUE(opened) << opened.error;
    const auto sourceFrame = sourceDecoder.videoFrame(0);
    ASSERT_TRUE(sourceFrame) << sourceFrame.error;
    ASSERT_TRUE(sourceFrame.value.source);
    const AVFrame* input = sourceFrame.value.source->get();
    ASSERT_NE(input, nullptr);
    ASSERT_EQ(input->format, AV_PIX_FMT_YUV420P);
    ASSERT_EQ(input->width, 160); ASSERT_EQ(input->height, 120);
    ASSERT_EQ(input->colorspace, AVCOL_SPC_BT709);
    ASSERT_EQ(input->color_range, AVCOL_RANGE_MPEG);
    std::array<std::array<double, 3>, 8> sourcePatches{};
    for (int patch = 0; patch < 8; ++patch) {
        const int x = patch * 20 + 10, y = 60;
        const double luma = (input->data[0][y * input->linesize[0] + x] - 16.0) / 219.0;
        const double cb = (input->data[1][(y / 2) * input->linesize[1] + x / 2] - 128.0) / 224.0;
        const double cr = (input->data[2][(y / 2) * input->linesize[2] + x / 2] - 128.0) / 224.0;
        // Independent limited-range BT.709 matrix; the explicit ACEScg
        // interpretation makes these unpacked RGB values scene-linear.
        sourcePatches[patch] = {luma + 1.5748 * cr,
            luma - (0.0722 * 1.8556 / 0.7152) * cb - (0.2126 * 1.5748 / 0.7152) * cr,
            luma + 1.8556 * cb};
    }
    constexpr double opacity = 0.65;
    for (const auto codec : {ExportVideoCodec::H264, ExportVideoCodec::H265}) {
        for (const std::string mode : {"normal", "multiply", "screen", "overlay", "add", "difference", "luminosity"}) {
            SCOPED_TRACE(mode + (codec == ExportVideoCodec::H264 ? " H264" : " H265"));
            ASSERT_TRUE(parseBlendMode(mode));
            OutputFolder folder;
            ProjectFile document;
            Timeline timeline;
            timeline.id = "blend-timeline"; timeline.width = 160; timeline.height = 120; timeline.fps = 24;
            Track upper; upper.id = "upper"; upper.muted = true;
            Clip source; source.id = "source"; source.mediaRef = "gray"; source.durationFrames = 6;
            source.transform.flipHorizontal = true;
            source.blendMode = mode; source.opacity = opacity;
            upper.clips.push_back(source);
            Track lower; lower.id = "lower"; lower.muted = true;
            Clip backdrop; backdrop.id = "backdrop"; backdrop.mediaRef = "gray"; backdrop.durationFrames = 6;
            lower.clips.push_back(backdrop);
            timeline.tracks = {upper, lower};
            document.timelines = {timeline}; document.activeTimelineId = timeline.id;
            // Deliberate interpretation: code values represent linear ACEScg,
            // not the fixture's tagged SDR transfer function. No IDT math is
            // borrowed from the renderer to calculate the blend reference.
            document.colorSettings.inputColorSpaces["gray"] = "ACEScg";
            document.colorSettings.exportOutput = {color::kExportDisplay, color::kColorimetricView};
            MediaManifest manifest;
            MediaManifestEntry media; media.id = "gray"; media.duration = 0.25;
            media.source.path = HEISENBERG_TEST_SDR_GRAY;
            ASSERT_TRUE(manifest.add(media));
            VideoProject project;
            std::string error;
            ASSERT_TRUE(project.editor().replaceDocument(document, manifest, "", &error)) << error;
            ExportRequest request; request.snapshot = project.snapshot(); request.outputPath = utf8(folder.output());
            request.hardwareDecode = false; request.videoCodec = codec; request.crf = 0;
            ExportEngine engine;
            ASSERT_TRUE(engine.start(request, &error)) << error; engine.wait();
            ASSERT_TRUE(engine.result().succeeded()) << engine.result().stage << ": " << engine.result().error;
            EXPECT_TRUE(engine.result().warnings.empty());
            const auto decoded = readback(folder.output(), codec == ExportVideoCodec::H264 ? AV_CODEC_ID_H264 : AV_CODEC_ID_HEVC, 6);
            ASSERT_EQ(decoded.pictures, 6);
            ASSERT_EQ(decoded.grayPatches.size(), 6u);
            for (int patch = 0; patch < 8; ++patch) {
                const auto& backdropRgb = sourcePatches[patch];
                const auto& sourceRgb = sourcePatches[7 - patch];
                auto blendRgb = sourceRgb;
                for (int channel = 0; channel < 3; ++channel) {
                    const double cb = backdropRgb[channel], cs = sourceRgb[channel];
                    if (mode == "multiply") blendRgb[channel] = cb * cs;
                    else if (mode == "screen") blendRgb[channel] = cb + cs - cb * cs;
                    else if (mode == "overlay") blendRgb[channel] = cb <= 0.5 ? 2 * cb * cs : 1 - 2 * (1 - cb) * (1 - cs);
                    else if (mode == "add") blendRgb[channel] = cb + cs;
                    else if (mode == "difference") blendRgb[channel] = std::abs(cb - cs);
                }
                if (mode == "luminosity") {
                    auto boundedBackdrop = backdropRgb, boundedSource = sourceRgb;
                    for (int channel = 0; channel < 3; ++channel) {
                        boundedBackdrop[channel] = std::clamp(boundedBackdrop[channel], 0.0, 1.0);
                        boundedSource[channel] = std::clamp(boundedSource[channel], 0.0, 1.0);
                    }
                    const auto lum = [](const auto& rgb) { return 0.3 * rgb[0] + 0.59 * rgb[1] + 0.11 * rgb[2]; };
                    const double delta = lum(boundedSource) - lum(boundedBackdrop);
                    for (int channel = 0; channel < 3; ++channel) blendRgb[channel] = boundedBackdrop[channel] + delta;
                    const double l = lum(blendRgb);
                    const double low = *std::min_element(blendRgb.begin(), blendRgb.end());
                    const double high = *std::max_element(blendRgb.begin(), blendRgb.end());
                    if (low < 0.0) for (auto& value : blendRgb) value = l + (value - l) * l / (l - low);
                    if (high > 1.0) for (auto& value : blendRgb) value = l + (value - l) * (1 - l) / (high - l);
                }
                float expected[4]{0, 0, 0, 1};
                for (int channel = 0; channel < 3; ++channel)
                    expected[channel] = static_cast<float>((1 - opacity) * backdropRgb[channel] + opacity * blendRgb[channel]);
                // The blend oracle above is independent. OCIO CPU supplies only
                // the output display transform, which is not a blend operation.
                toOutput.applyRGBA(expected, 1);
                const double luma = 16 + 219 * (0.2126 * std::clamp(expected[0], 0.0f, 1.0f)
                    + 0.7152 * std::clamp(expected[1], 0.0f, 1.0f)
                    + 0.0722 * std::clamp(expected[2], 0.0f, 1.0f));
                for (size_t frame = 0; frame < decoded.grayPatches.size(); ++frame)
                    EXPECT_NEAR(decoded.grayPatches[frame][patch], luma, 3.0) << "frame=" << frame << " patch=" << patch;
            }
            EXPECT_EQ(engine.progress().completedFrames, 6);
            EXPECT_TRUE(folder.noStaging());
        }
    }
}

TEST(ExportEngineTest, UnknownBlendPreflightPreservesTargetsEvenOnInactiveVideoTracks) {
    OutputFolder folder;
    { std::ofstream output(folder.output(), std::ios::binary); output << "keep-original-blend-target"; }
    const auto modified = fs::last_write_time(folder.output());
    const auto initial = snapshot(2, true, false);
    for (const std::string mode : {"unknown", "", "Normal", "colorburn"}) {
        for (const int state : {0, 1, 2, 3}) {
            SCOPED_TRACE(mode + ":" + std::to_string(state));
            auto document = initial.project();
            auto later = document.timelines.front().tracks.front(); later.id = "later-video";
            later.hidden = state == 1; later.muted = state == 2;
            auto& clip = later.clips.front(); clip.id = "unknown-picture"; clip.blendMode = mode;
            clip.opacity = state == 3 ? 0.0 : 1.0;
            document.timelines.front().tracks.push_back(later);
            VideoProject project;
            std::string error;
            ASSERT_TRUE(project.editor().replaceDocument(document, initial.manifest(), "", &error)) << error;
            EXPECT_EQ(project.editor().compatibility().level, CompatibilityLevel::Warning);
            for (const auto& target : {folder.output(), folder.root / "absent" / "output.mp4"}) {
                ExportRequest request; request.snapshot = project.snapshot();
                request.outputPath = utf8(target); request.overwrite = true;
                ExportEngine engine;
                ASSERT_TRUE(engine.start(request, &error)) << error; engine.wait();
                EXPECT_EQ(engine.result().state, ExportState::Failed);
                EXPECT_EQ(engine.result().stage, "prepare");
                EXPECT_NE(engine.result().error.find("blend"), std::string::npos);
                EXPECT_NE(engine.result().error.find("unknown-picture"), std::string::npos);
                EXPECT_EQ(engine.progress().completedFrames, 0);
                EXPECT_EQ(contents(folder.output()), "keep-original-blend-target");
                EXPECT_EQ(fs::last_write_time(folder.output()), modified);
                EXPECT_FALSE(fs::exists(folder.root / "absent"));
                EXPECT_TRUE(folder.noStaging());
            }
        }
    }
}

TEST(ExportEngineTest, AudioTailAndAudioOnlyProduceBlackFramesAndKeepSound) {
    for (bool picture : {true, false}) {
        OutputFolder folder; ExportEngine engine; ExportRequest request;
        request.snapshot = snapshot(72, picture); request.outputPath = utf8(folder.output());
        ASSERT_TRUE(engine.start(request)); engine.wait();
        ASSERT_TRUE(engine.result().succeeded()) << engine.result().stage << ": " << engine.result().error;
        const auto decoded = readback(folder.output(), AV_CODEC_ID_H264, 72);
        EXPECT_EQ(decoded.pictures, 72);
        EXPECT_GT(tone(decoded.audio, 880, 1.5, 2.5), .03);
        EXPECT_LT(tone(decoded.audio, 440, 1.5, 2.5), .005);
        for (size_t i = picture ? 24 : 0; i < decoded.brightness.size(); ++i)
            EXPECT_NEAR(decoded.brightness[i], 16, 2.0);
        EXPECT_TRUE(folder.noStaging());
    }
}

TEST(ExportEngineTest, PlanarGeometrySurvivesReopenAndOwnedSnapshotThenFullyDecodes) {
    OutputFolder folder;
    VideoProject project;
    ProjectFile document;
    Timeline timeline;
    timeline.id = "geometry-timeline"; timeline.width = 160; timeline.height = 120; timeline.fps = 24;
    Track upper; upper.id = "upper"; upper.muted = true;
    Clip shape; shape.id = "transformed"; shape.mediaRef = "gray";
    shape.startFrame = 2; shape.durationFrames = 4; shape.trimStartFrame = 1; shape.trimEndFrame = 1;
    shape.transform.centerX = 0.55; shape.transform.centerY = 0.55;
    shape.transform.width = 0.75; shape.transform.height = 0.5;
    shape.transform.rotation = 90; shape.transform.flipHorizontal = true; shape.transform.flipVertical = true;
    shape.crop = {0.125, 0.1, 0.125, 0.2};
    shape.edgeRounding = 0.8; shape.edgeSoftness = 0.5;
    upper.clips.push_back(shape);
    Track lower; lower.id = "lower"; lower.muted = true;
    Clip background; background.id = "background"; background.mediaRef = "gray";
    background.durationFrames = 12; background.speed = 0.5;
    lower.clips.push_back(background);
    timeline.tracks = {upper, lower};
    document.timelines.push_back(timeline); document.activeTimelineId = timeline.id;
    document.colorSettings.inputColorSpaces["gray"] = "Rec.1886 Rec.709 - Display";
    MediaManifest manifest;
    MediaManifestEntry media; media.id = "gray"; media.duration = 0.25;
    media.source.path = HEISENBERG_TEST_SDR_GRAY;
    ASSERT_TRUE(manifest.add(media));
    std::string error;
    ASSERT_TRUE(project.editor().replaceDocument(document, manifest, "", &error)) << error;
    const auto saved = project.snapshot();
    const auto package = folder.root / "geometry.berg";
    ASSERT_TRUE(project.savePackage(utf8(package), &error)) << error;
    VideoProject reopened;
    ASSERT_TRUE(reopened.loadPackage(utf8(package), &error)) << error;
    const auto owned = reopened.snapshot();
    EXPECT_EQ(owned.project(), saved.project());
    EXPECT_EQ(owned.manifest(), saved.manifest());
    ASSERT_NE(owned.activeTimeline(), nullptr);
    EXPECT_EQ(owned.activeTimeline()->id, "geometry-timeline");
    EXPECT_EQ(owned.activeTimeline()->tracks.front().clips.front(), shape);
    EXPECT_EQ(owned.activeTimeline()->tracks.back().clips.front(), background);
    CompositionPlan plan;
    ASSERT_TRUE(CompositionBuilder::build(owned, plan, &error)) << error;
    ASSERT_EQ(plan.tracks.size(), 2u);
    EXPECT_EQ(plan.frameCount, 12);
    EXPECT_EQ(plan.tracks[0].id, "upper"); EXPECT_EQ(plan.tracks[1].id, "lower");
    EXPECT_EQ(plan.tracks[0].clips[0].visualSnapshot, shape);
    EXPECT_EQ(plan.tracks[1].clips[0].visualSnapshot, background);
    for (int f = 0; f < 12; ++f) {
        const auto layers = plan.videoSamplesAt(f);
        ASSERT_EQ(layers.size(), f >= 2 && f < 6 ? 2u : 1u);
        for (const auto& layer : layers) {
            const bool top = layer.trackIndex == 0;
            EXPECT_EQ(layer.clipLocalFrame, top ? f - 2 : f);
            EXPECT_NEAR(layer.sourceSeconds, (top ? f - 1.0 : f * 0.5) / 24.0, 1e-9);
        }
    }
    // Mutate both the reopened editor and caller's document after capturing.
    // The export must still use the saved geometry, IDs and frame ranges.
    auto changed = owned.project();
    changed.timelines.front().tracks.front().clips.front().transform = {};
    changed.timelines.front().tracks.front().clips.front().crop = {};
    changed.timelines.front().tracks.front().clips.front().edgeRounding = 0;
    changed.timelines.front().tracks.front().clips.front().edgeSoftness = 0;
    changed.timelines.front().tracks.front().clips.front().startFrame = 0;
    ASSERT_TRUE(reopened.editor().replaceDocument(changed, owned.manifest(), owned.projectDir(), &error)) << error;
    EXPECT_EQ(owned.activeTimeline()->tracks.front().clips.front(), shape);
    for (auto codec : {ExportVideoCodec::H264, ExportVideoCodec::H265}) {
        ExportEngine engine;
        ExportRequest request; request.snapshot = owned; request.outputPath = utf8(folder.output());
        request.overwrite = true; request.hardwareDecode = false; request.videoCodec = codec; request.crf = 0;
        ASSERT_TRUE(engine.start(request, &error)) << error; engine.wait();
        ASSERT_TRUE(engine.result().succeeded()) << engine.result().stage << ": " << engine.result().error;
        const auto decoded = readback(folder.output(), codec == ExportVideoCodec::H264 ? AV_CODEC_ID_H264 : AV_CODEC_ID_HEVC, 12);
        ASSERT_EQ(decoded.luma.size(), 12u); ASSERT_EQ(decoded.videoTimes.size(), 12u);
        EXPECT_EQ(engine.progress().completedFrames, 12);
        EXPECT_EQ(engine.progress().completedSamples, 24000);
        const double levels[]{16, 24, 40, 64, 96, 128, 192, 235};
        int interior = 0, feather = 0, outside = 0;
        for (int f = 0; f < 12; ++f) {
            EXPECT_NEAR(decoded.videoTimes[f], f / 24.0, 1e-6);
            for (int y = 5; y < 115; y += 3) for (int x = 5; x < 155; x += 3) {
                if (x % 20 < 4 || x % 20 > 16) continue;
                const double bottom = levels[x / 20];
                double expected = bottom;
                if (f >= 2 && f < 6) {
                    // Independent inverse of clockwise 90 degree placement, then
                    // both source flips. Width/height multiply the 160x120 fit.
                    const double sx = 80 - (y + 0.5 - 66) / 0.75;
                    const double sy = 60 + (x + 0.5 - 88) / 0.5;
                    // Crop rect [20,12,140,96], radius .8*84/2, feather .5*84/2.
                    const double qx = std::abs(sx - 80) - (60 - 33.6);
                    const double qy = std::abs(sy - 54) - (42 - 33.6);
                    const double distance = std::hypot(std::max(qx, 0.0), std::max(qy, 0.0))
                        + std::min(std::max(qx, qy), 0.0) - 33.6;
                    const double t = std::clamp((distance + 21.5) / 22.0, 0.0, 1.0);
                    const double alpha = 1 - t * t * (3 - 2 * t);
                    if (alpha > 0) {
                        if (sx < 4 || sx >= 156 || std::fmod(sx, 20.0) < 4 || std::fmod(sx, 20.0) > 16) continue;
                        const double top = levels[static_cast<int>(sx) / 20];
                        const double linear = alpha * std::pow((top - 16) / 219, 2.4)
                            + (1 - alpha) * std::pow((bottom - 16) / 219, 2.4);
                        expected = 16 + 219 * std::pow(linear, 1 / 2.4);
                    }
                    if (alpha > 0.98) ++interior;
                    else if (alpha > 0.15 && alpha < 0.85) ++feather;
                    else if (alpha == 0) ++outside;
                }
                EXPECT_NEAR(geometryPatch(decoded.luma[f], x, y), expected, 7.0)
                    << "frame=" << f << " pixel=" << x << ',' << y;
            }
        }
        EXPECT_GT(interior, 20); EXPECT_GT(feather, 20); EXPECT_GT(outside, 100);
        EXPECT_TRUE(folder.noStaging());
    }
}

TEST(ExportEngineTest, FiveAppearanceTracksSurviveReopenImmutableSnapshotAndBothEncoders) {
    namespace color = heisenberg::renderer::color;
    color::ColorPipeline pipeline;
    const auto toOutput = pipeline.displayProcessor(color::kWorkingSpace,color::kExportDisplay,color::kColorimetricView);
    // Decode the static gray fixture, then independently unpack limited BT.709.
    // OCIO below supplies only the output transform, never animation or geometry.
    CompositionExecutor decoder;
    const auto opened = decoder.openSource(HEISENBERG_TEST_SDR_GRAY,{false,CompositionErrorPolicy::Strict});
    ASSERT_TRUE(opened) << opened.error;
    const auto decodedSource = decoder.videoFrame(0); ASSERT_TRUE(decodedSource) << decodedSource.error;
    ASSERT_TRUE(decodedSource.value.source);
    const AVFrame* input = decodedSource.value.source->get(); ASSERT_NE(input,nullptr);
    ASSERT_EQ(input->format,AV_PIX_FMT_YUV420P);
    ASSERT_EQ(input->width,160); ASSERT_EQ(input->height,120);
    ASSERT_EQ(input->colorspace,AVCOL_SPC_BT709);
    ASSERT_EQ(input->color_range,AVCOL_RANGE_MPEG);
    using Rgba=std::array<double,4>;
    std::vector<Rgba> sourceRgb(160*120);
    for (int y=0;y<120;++y) for (int x=0;x<160;++x) {
        const double luma=(input->data[0][y*input->linesize[0]+x]-16.0)/219;
        const double cb=(input->data[1][y/2*input->linesize[1]+x/2]-128.0)/224;
        const double cr=(input->data[2][y/2*input->linesize[2]+x/2]-128.0)/224;
        sourceRgb[y*160+x]={luma+1.5748*cr,luma-(0.0722*1.8556/0.7152)*cb-(0.2126*1.5748/0.7152)*cr,luma+1.8556*cb,1};
    }
    // Independent, normalized Lanczos-3 convolution of premultiplied source
    // texels. Shrinking widens the kernel in source coordinates. Edge extension
    // belongs to this prefilter; the final affine reconstruction has zero borders.
    // No renderer, libplacebo filter LUT or appearance sampler supplies the oracle.
    const auto shrink=[](const std::vector<Rgba>& pixels,int w,int h,int outputSize,bool horizontal) {
        const int inputSize=horizontal ? w : h;
        const int outputW=horizontal ? outputSize : w,outputH=horizontal ? h : outputSize;
        std::vector<Rgba> result(outputW*outputH);
        const double scale=double(inputSize)/outputSize;
        const auto sinc=[](double x) {
            if (std::abs(x)<1e-12) return 1.0;
            const double angle=3.14159265358979323846*x;
            return std::sin(angle)/angle;
        };
        for (int out=0;out<outputSize;++out) {
            const double center=(out+0.5)*scale-0.5;
            std::vector<std::pair<int,double>> taps;
            double total=0;
            for (int tap=int(std::ceil(center-3*scale));tap<=int(std::floor(center+3*scale));++tap) {
                const double d=(tap-center)/scale;
                const double weight=std::abs(d)<3 ? sinc(d)*sinc(d/3) : 0;
                taps.emplace_back(std::clamp(tap,0,inputSize-1),weight); total+=weight;
            }
            for (int other=0;other<(horizontal ? h : w);++other) {
                auto& pixel=result[horizontal ? other*outputW+out : out*outputW+other];
                for (const auto& [tap,weight] : taps) {
                    const auto& sample=pixels[horizontal ? other*w+tap : tap*w+other];
                    for (int c=0;c<4;++c) pixel[c]+=sample[c]*weight/total;
                }
            }
        }
        return result;
    };
    for (const std::string interpolation : {"linear","hold","smooth"}) {
        OutputFolder folder;
        const auto keys = [&](JsonValue a,JsonValue b,JsonValue c) {
            return KeyframeTrackPayload{JsonValue{{"keyframes",JsonValue::array({
                JsonValue{{"frame",2},{"value",a},{"interpolationOut",interpolation}},
                JsonValue{{"frame",10},{"value",b},{"interpolationOut",interpolation}},
                JsonValue{{"frame",14},{"value",c}}})}}};
        };
        Clip shape; shape.id="animated"; shape.mediaRef="gray";
        shape.startFrame=2; shape.durationFrames=16; shape.trimStartFrame=1; shape.trimEndFrame=1; shape.speed=0.25;
        shape.opacity=0; shape.transform.centerX=0.1; shape.transform.centerY=0.9;
        shape.transform.width=1.8; shape.transform.height=1.6; shape.transform.rotation=-123;
        shape.transform.flipHorizontal=true; shape.edgeRounding=0.4; shape.edgeSoftness=0.3;
        shape.opacityTrack=keys(0.0,0.9,0.45);
        shape.positionTrack=keys(JsonValue{{"a",0.4},{"b",0.45}},JsonValue{{"a",0.6},{"b",0.55}},JsonValue{{"a",0.45},{"b",0.4}});
        shape.scaleTrack=keys(JsonValue{{"a",0.7},{"b",0.9}},JsonValue{{"a",1.1},{"b",0.7}},JsonValue{{"a",0.85},{"b",1.05}});
        shape.rotationTrack=keys(350.0,10.0,-50.0);
        shape.cropTrack=keys(JsonValue{{"left",0.05},{"top",0.1},{"right",0.2},{"bottom",0.05}},
            JsonValue{{"left",0.2},{"top",0.05},{"right",0.05},{"bottom",0.2}},
            JsonValue{{"left",0.1},{"top",0.2},{"right",0.15},{"bottom",0.1}});
        ProjectFile document;
        Timeline timeline; timeline.id="appearance-animation"; timeline.width=160; timeline.height=120; timeline.fps=24;
        Track track; track.id="video"; track.muted=true; track.clips.push_back(shape);
        timeline.tracks.push_back(track); document.timelines.push_back(timeline); document.activeTimelineId=timeline.id;
        document.colorSettings.inputColorSpaces["gray"]="ACEScg";
        document.colorSettings.exportOutput={color::kExportDisplay,color::kColorimetricView};
        MediaManifest manifest; MediaManifestEntry media; media.id="gray"; media.duration=0.25;
        media.source.path=HEISENBERG_TEST_SDR_GRAY; ASSERT_TRUE(manifest.add(media));
        VideoProject project; std::string error;
        ASSERT_TRUE(project.editor().replaceDocument(document,manifest,"",&error)) << error;
        EXPECT_EQ(project.editor().compatibility().level,CompatibilityLevel::Supported);
        const auto saved=project.snapshot();
        const auto package=folder.root/"animation.berg";
        ASSERT_TRUE(project.savePackage(utf8(package),&error)) << error;
        VideoProject reopened; ASSERT_TRUE(reopened.loadPackage(utf8(package),&error)) << error;
        const auto owned=reopened.snapshot();
        EXPECT_EQ(owned.project(),saved.project()); EXPECT_EQ(owned.manifest(),saved.manifest());
        ASSERT_NE(owned.activeTimeline(),nullptr);
        EXPECT_EQ(owned.activeTimeline()->tracks.front().clips.front(),shape);
        CompositionPlan plan; ASSERT_TRUE(CompositionBuilder::build(owned,plan,&error)) << error;
        EXPECT_EQ(plan.frameCount,18);
        for (int f=0;f<18;++f) {
            const auto sample=plan.videoSampleAt(f);
            if (f<2) { EXPECT_FALSE(sample); continue; }
            ASSERT_TRUE(sample); EXPECT_EQ(sample->clipLocalFrame,f-2);
            EXPECT_NEAR(sample->sourceSeconds,(1+(f-2)*0.25)/24.0,1e-9);
        }
        auto changed=owned.project();
        auto& edit=changed.timelines.front().tracks.front().clips.front();
        edit.opacityTrack.reset(); edit.positionTrack.reset(); edit.scaleTrack.reset();
        edit.rotationTrack.reset(); edit.cropTrack.reset(); edit.opacity=0; edit.transform={};
        ASSERT_TRUE(reopened.editor().replaceDocument(changed,owned.manifest(),owned.projectDir(),&error)) << error;
        changed.timelines.clear(); // Neither editor nor caller can mutate the export capture.
        EXPECT_EQ(owned.activeTimeline()->tracks.front().clips.front(),shape);
        for (const auto codec : {ExportVideoCodec::H264,ExportVideoCodec::H265}) {
            SCOPED_TRACE(interpolation+(codec==ExportVideoCodec::H264 ? " H264" : " H265"));
            ExportRequest request; request.snapshot=owned; request.outputPath=utf8(folder.output());
            request.overwrite=true; request.hardwareDecode=false; request.videoCodec=codec; request.crf=0;
            ExportEngine engine; ASSERT_TRUE(engine.start(request,&error)) << error; engine.wait();
            ASSERT_TRUE(engine.result().succeeded()) << engine.result().stage << ": " << engine.result().error;
            EXPECT_TRUE(engine.result().warnings.empty());
            const auto decoded=readback(folder.output(),codec==ExportVideoCodec::H264 ? AV_CODEC_ID_H264 : AV_CODEC_ID_HEVC,18);
            ASSERT_EQ(decoded.luma.size(),18u); ASSERT_EQ(decoded.videoTimes.size(),18u);
            EXPECT_EQ(engine.progress().completedFrames,18); EXPECT_EQ(engine.progress().completedSamples,36000);
            int interior=0,feather=0,outside=0;
            for (int f=0;f<18;++f) {
                EXPECT_NEAR(decoded.videoTimes[f],f/24.0,1e-6);
                // Independent fixed endpoint/time oracle: clip-local timeline
                // frames exclude trim and speed. Hold jumps at the right key.
                const int local=f-2; const bool second=local>=10;
                double t=std::clamp((local-(second ? 10.0 : 2.0))/(second ? 4.0 : 8.0),0.0,1.0);
                if (interpolation=="hold" && t<1) t=0;
                if (interpolation=="smooth") t=t*t*(3-2*t);
                const auto mix=[&](double a,double b,double c) { return second ? b+(c-b)*t : a+(b-a)*t; };
                const double opacity=mix(0,0.9,0.45),cx=160*mix(0.4,0.6,0.45),cy=120*mix(0.45,0.55,0.4);
                const double width=mix(0.7,1.1,0.85),height=mix(0.9,0.7,1.05);
                const double angle=mix(350,10,-50)*3.14159265358979323846/180;
                const double left=160*mix(0.05,0.2,0.1),right=160*(1-mix(0.2,0.05,0.15));
                const double top=120*mix(0.1,0.05,0.2),bottom=120*(1-mix(0.05,0.2,0.1));
                const double shortSide=std::min(right-left,bottom-top),radius=0.4*shortSide/2,softness=0.3*shortSide/2;
                auto premultiplied=sourceRgb;
                for (int sy=0;sy<120;++sy) for (int sx=0;sx<160;++sx) {
                    const double qx=std::abs(sx+0.5-(left+right)/2)-(right-left)/2+radius;
                    const double qy=std::abs(sy+0.5-(top+bottom)/2)-(bottom-top)/2+radius;
                    const double distance=std::hypot(std::max(qx,0.0),std::max(qy,0.0))+std::min(std::max(qx,qy),0.0)-radius;
                    const double maskT=std::clamp((distance+0.5+softness)/(1+softness),0.0,1.0);
                    const double coverage=1-maskT*maskT*(3-2*maskT);
                    auto& pixel=premultiplied[sy*160+sx];
                    for (auto& channel : pixel) channel*=coverage;
                }
                const int filteredW=std::clamp(int(std::ceil(160*width)),1,160);
                const int filteredH=std::clamp(int(std::ceil(120*height)),1,120);
                if (filteredW<160) premultiplied=shrink(premultiplied,160,120,filteredW,true);
                if (filteredH<120) premultiplied=shrink(premultiplied,filteredW,120,filteredH,false);
                const auto expectedPixel=[&](int x,int y,double* amount=nullptr) {
                    const double dx=x+0.5-cx,dy=y+0.5-cy;
                    const double u=0.5-(std::cos(angle)*dx+std::sin(angle)*dy)/(160*width);
                    const double v=0.5+(-std::sin(angle)*dx+std::cos(angle)*dy)/(120*height);
                    // Half-texel coordinates in the FILTERED source, followed by
                    // a zero-border bilinear reconstruction of premultiplied RGBA.
                    const double sx=u*filteredW-0.5,sy=v*filteredH-0.5;
                    Rgba sample{};
                    if (sx>-1 && sy>-1 && sx<filteredW && sy<filteredH) {
                        const int ix=int(std::floor(sx)),iy=int(std::floor(sy));
                        const double fx=sx-ix,fy=sy-iy;
                        for (int j=0;j<2;++j) for (int i=0;i<2;++i) {
                            const int xx=ix+i,yy=iy+j;
                            if (xx<0 || yy<0 || xx>=filteredW || yy>=filteredH) continue;
                            const double weight=(i ? fx : 1-fx)*(j ? fy : 1-fy);
                            const auto& tap=premultiplied[yy*filteredW+xx];
                            for (int c=0;c<4;++c) sample[c]+=tap[c]*weight;
                        }
                    }
                    const double alpha=f<2 ? 0 : opacity*std::clamp(sample[3],0.0,1.0);
                    if (amount) *amount=alpha;
                    float expected[4]{0,0,0,1};
                    if (alpha>0) for (int c=0;c<3;++c)
                        expected[c]=static_cast<float>(sample[c]/sample[3]*alpha);
                    // Transform EACH output pixel before averaging encoded luma.
                    // Averaging alpha/RGB first is wrong at soft edges because
                    // the display transfer is nonlinear, particularly near black.
                    toOutput.applyRGBA(expected,1);
                    return 16+219*(0.2126*std::clamp(expected[0],0.0f,1.0f)
                        +0.7152*std::clamp(expected[1],0.0f,1.0f)+0.0722*std::clamp(expected[2],0.0f,1.0f));
                };
                for (int y=5;y<115;y+=4) for (int x=5;x<155;x+=4) {
                    double alpha=0;
                    expectedPixel(x,y,&alpha);
                    if (alpha>0.7) ++interior;
                    else if (alpha>0.1 && alpha<0.6) ++feather;
                    else if (alpha==0) ++outside;
                    double luma=0;
                    for (int dy=-1;dy<=1;++dy) for (int dx=-1;dx<=1;++dx)
                        luma+=expectedPixel(x+dx,y+dy)/9;
                    EXPECT_NEAR(geometryPatch(decoded.luma[f],x,y),luma,8.0) << "frame=" << f << " pixel=" << x << ',' << y;
                }
            }
            EXPECT_GT(interior,20); EXPECT_GT(feather,20); EXPECT_GT(outside,100);
            EXPECT_NEAR(decoded.brightness[2],16,2);
            EXPECT_GT(decoded.brightness[12],decoded.brightness[2]+10); // Actual fade-in from a zero constant.
            EXPECT_GT(std::abs(decoded.brightness[16]-decoded.brightness[12]),1);
            EXPECT_TRUE(folder.noStaging());
        }
    }
}

TEST(ExportEngineTest, NonSquareSourceKeepsDefaultAspectFitAndScalesFromFittedDimensions) {
    // Existing 160x90 source fits a 160x120 canvas at y=[15,105].
    // Halving each size must produce 80x45, rather than 80x60.
    OutputFolder folder;
    for (const double scale : {1.0, 0.5}) {
        const auto initial = snapshot(2, true, false);
        auto document = initial.project();
        auto& clip = document.timelines.front().tracks.front().clips.front();
        clip.transform.width = clip.transform.height = scale;
        MediaManifest manifest;
        MediaManifestEntry media; media.id = "v"; media.duration = 10;
        media.source.path = HEISENBERG_TEST_EXECUTOR_LOOKAHEAD;
        ASSERT_TRUE(manifest.add(media));
        VideoProject project;
        std::string error;
        ASSERT_TRUE(project.editor().replaceDocument(document, manifest, "", &error)) << error;
        ExportRequest request; request.snapshot = project.snapshot(); request.outputPath = utf8(folder.output());
        request.overwrite = true; request.hardwareDecode = false; request.crf = 0;
        ExportEngine engine;
        ASSERT_TRUE(engine.start(request, &error)) << error; engine.wait();
        ASSERT_TRUE(engine.result().succeeded()) << engine.result().stage << ": " << engine.result().error;
        const auto decoded = readback(folder.output(), AV_CODEC_ID_H264, 2);
        ASSERT_EQ(decoded.luma.size(), 2u);
        for (const auto& image : decoded.luma) {
            const int top = scale == 1.0 ? 15 : 37;
            const int bottom = scale == 1.0 ? 105 : 83;
            for (int y = 2; y < top - 2; ++y)
                for (int x = 2; x < 158; ++x) EXPECT_NEAR(image[y * 160 + x], 16, 2);
            for (int y = bottom + 2; y < 118; ++y)
                for (int x = 2; x < 158; ++x) EXPECT_NEAR(image[y * 160 + x], 16, 2);
            double upperContent = 0, lowerContent = 0;
            for (int x = 50; x < 110; ++x) {
                upperContent += image[(top + 5) * 160 + x];
                lowerContent += image[(bottom - 6) * 160 + x];
            }
            EXPECT_GT(upperContent / 60, 35);
            EXPECT_GT(lowerContent / 60, 35);
            if (scale == 0.5) {
                EXPECT_NEAR(geometryPatch(image, 30, 60), 16, 2);
                EXPECT_NEAR(geometryPatch(image, 130, 60), 16, 2);
            }
        }
        EXPECT_TRUE(folder.noStaging());
    }
}

TEST(ExportEngineTest, GeometryPreflightRejectsInvalidConstantsAndPerspectiveBeforeOutputAccess) {
    struct Case {
        const char* name;
        std::function<void(Clip&)> apply;
        const char* diagnostic;
    };
    const std::vector<Case> cases{
        {"zero width", [](Clip& c) { c.transform.width = 0; }, "Invalid clip appearance"},
        {"negative height", [](Clip& c) { c.transform.height = -1; }, "Invalid clip appearance"},
        {"negative crop", [](Clip& c) { c.crop.left = -0.1; }, "Invalid clip appearance"},
        {"crop above one", [](Clip& c) { c.crop.bottom = 1.1; }, "Invalid clip appearance"},
        {"empty crop x", [](Clip& c) { c.crop.left = c.crop.right = 0.5; }, "Invalid clip appearance"},
        {"empty crop y", [](Clip& c) { c.crop.top = 0.7; c.crop.bottom = 0.4; }, "Invalid clip appearance"},
        {"negative rounding", [](Clip& c) { c.edgeRounding = -0.1; }, "Invalid clip appearance"},
        {"excess softness", [](Clip& c) { c.edgeSoftness = 1.1; }, "Invalid clip appearance"},
        {"perspective x", [](Clip& c) { c.transform.rotationX = 1; }, "Unsupported clip perspective rotation"},
        {"perspective y", [](Clip& c) { c.transform.rotationY = -1; }, "Unsupported clip perspective rotation"},
        {"blend", [](Clip& c) { c.blendMode = "unknown"; }, "Unsupported clip blend mode"},
    };
    OutputFolder folder;
    { std::ofstream output(folder.output(), std::ios::binary); output << "keep-original-geometry-target"; }
    const auto modified = fs::last_write_time(folder.output());
    const auto initial = snapshot(2, true, false);
    for (const auto& test : cases) {
        SCOPED_TRACE(test.name);
        auto document = initial.project();
        test.apply(document.timelines.front().tracks.front().clips.front());
        VideoProject project;
        std::string error;
        ASSERT_TRUE(project.editor().replaceDocument(document, initial.manifest(), "", &error)) << error;
        EXPECT_EQ(project.editor().compatibility().level, CompatibilityLevel::Warning);
        // A missing output parent would fail staging first if preflight ran late.
        for (const auto& target : {folder.output(), folder.root / "absent" / "output.mp4"}) {
            ExportRequest request; request.snapshot = project.snapshot();
            request.outputPath = utf8(target); request.overwrite = true; request.hardwareDecode = false;
            ExportEngine engine;
            ASSERT_TRUE(engine.start(request, &error)) << error; engine.wait();
            EXPECT_EQ(engine.result().state, ExportState::Failed);
            EXPECT_EQ(engine.result().stage, "prepare");
            EXPECT_NE(engine.result().error.find(test.diagnostic), std::string::npos) << engine.result().error;
            EXPECT_NE(engine.result().error.find("picture"), std::string::npos);
            EXPECT_EQ(engine.progress().completedFrames, 0);
            EXPECT_EQ(contents(folder.output()), "keep-original-geometry-target");
            EXPECT_EQ(fs::last_write_time(folder.output()), modified);
            EXPECT_FALSE(fs::exists(folder.root / "absent"));
            EXPECT_TRUE(folder.noStaging());
        }
    }
}

TEST(ExportEngineTest, InvalidAndFutureAppearanceTracksRemainSaveableButFailBeforeOutputAccess) {
    using TrackMember=std::optional<KeyframeTrackPayload> Clip::*;
    const std::array<TrackMember,5> members{&Clip::opacityTrack,&Clip::positionTrack,&Clip::scaleTrack,&Clip::rotationTrack,&Clip::cropTrack};
    OutputFolder folder;
    { std::ofstream output(folder.output(),std::ios::binary); output << "keep-animation-target"; }
    const auto modified=fs::last_write_time(folder.output());
    const auto initial=snapshot(2,true,false);
    for (size_t index=0;index<members.size();++index) {
        JsonValue value=0.5;
        if (index==1 || index==2) value=JsonValue{{"a",0.5},{"b",0.75}};
        if (index==4) value=JsonValue{{"left",0.1},{"top",0.05},{"right",0.2},{"bottom",0.15}};
        const JsonValue valid{{"keyframes",JsonValue::array({JsonValue{{"frame",0},{"value",value}},
            JsonValue{{"frame",20},{"value",value}}})}};
        std::vector<JsonValue> payloads{JsonValue::object(),JsonValue{{"futureVersion",99},{"curve","opaque"}}};
        auto future=valid; future["keyframes"][1]["interpolationOut"]="bezier"; payloads.push_back(future);
        auto bad=valid;
        if (index==0) bad["keyframes"][1]["value"]=-0.1;
        if (index==1) bad["keyframes"][1]["value"].erase("b");
        if (index==2) bad["keyframes"][1]["value"]["a"]=-1;
        if (index==3) bad["keyframes"][1]["value"]="angle";
        if (index==4) bad["keyframes"][1]["value"]["bottom"]=1.1;
        payloads.push_back(bad);
        for (size_t payload=0;payload<payloads.size();++payload) for (const int state : {0,1,2,3,4}) {
            SCOPED_TRACE(testing::Message() << "track=" << index << " payload=" << payload << " state=" << state);
            auto document=initial.project(); auto& track=document.timelines.front().tracks.front();
            auto& clip=track.clips.front(); clip.*members[index]=KeyframeTrackPayload{payloads[payload]};
            track.hidden=state==1; track.muted=state==4;
            clip.opacity=state==2 ? 0 : 1;
            if (state==3) clip.startFrame=8; // Export starts with a blank/inactive interval.
            VideoProject project; std::string error;
            ASSERT_TRUE(project.editor().replaceDocument(document,initial.manifest(),"",&error)) << error;
            EXPECT_EQ(project.editor().compatibility().level,CompatibilityLevel::Warning);
            const auto package=folder.root/("unknown-"+std::to_string(index)+"-"+std::to_string(payload)+"-"+std::to_string(state)+".berg");
            ASSERT_TRUE(project.savePackage(utf8(package),&error)) << error;
            VideoProject reopened; ASSERT_TRUE(reopened.loadPackage(utf8(package),&error)) << error;
            const auto owned=reopened.snapshot();
            ASSERT_NE(owned.activeTimeline(),nullptr);
            EXPECT_EQ((owned.activeTimeline()->tracks.front().clips.front().*members[index])->value,payloads[payload]);
            for (const auto& target : {folder.output(),folder.root/"absent"/"output.mp4"}) {
                ExportRequest request; request.snapshot=owned; request.outputPath=utf8(target); request.overwrite=true;
                ExportEngine engine; ASSERT_TRUE(engine.start(request,&error)) << error; engine.wait();
                EXPECT_EQ(engine.result().state,ExportState::Failed); EXPECT_EQ(engine.result().stage,"prepare");
                EXPECT_NE(engine.result().error.find("picture"),std::string::npos) << engine.result().error;
                EXPECT_FALSE(engine.result().error.empty()); EXPECT_EQ(engine.progress().completedFrames,0);
                EXPECT_EQ(contents(folder.output()),"keep-animation-target");
                EXPECT_EQ(fs::last_write_time(folder.output()),modified);
                EXPECT_FALSE(fs::exists(folder.root/"absent")); EXPECT_TRUE(folder.noStaging());
            }
        }
    }
}

TEST(ExportEngineTest, EmptyAppearanceTracksPassPreflightUsingConstants) {
    const auto initial=snapshot(2,true,false);
    auto document=initial.project(); auto& clip=document.timelines.front().tracks.front().clips.front();
    const KeyframeTrackPayload empty{JsonValue{{"keyframes",JsonValue::array()}}};
    clip.opacityTrack=clip.positionTrack=clip.scaleTrack=clip.rotationTrack=clip.cropTrack=empty;
    VideoProject project; std::string error;
    ASSERT_TRUE(project.editor().replaceDocument(document,initial.manifest(),"",&error)) << error;
    EXPECT_EQ(project.editor().compatibility().level,CompatibilityLevel::Supported);
    OutputFolder folder; ExportRequest request; request.snapshot=project.snapshot();
    request.outputPath=utf8(folder.root/"absent"/"output.mp4");
    ExportEngine engine; ASSERT_TRUE(engine.start(request,&error)) << error; engine.wait();
    EXPECT_EQ(engine.result().state,ExportState::Failed);
    EXPECT_EQ(engine.result().error,"Output directory does not exist");
    EXPECT_FALSE(fs::exists(folder.root/"absent")); EXPECT_TRUE(folder.noStaging());
}

TEST(ExportEngineTest, RefusesOverwriteAndInvalidSizeWithoutTouchingExistingTarget) {
    OutputFolder folder;
    { std::ofstream output(folder.output(), std::ios::binary); output << "original-target"; }
    for (bool invalidSize : {false, true}) {
        ExportEngine engine; ExportRequest request;
        request.snapshot = snapshot(24, true, true, invalidSize ? 161 : 160);
        request.outputPath = utf8(folder.output()); request.overwrite = invalidSize;
        ASSERT_TRUE(engine.start(request)); engine.wait();
        EXPECT_EQ(engine.result().state, ExportState::Failed);
        EXPECT_FALSE(engine.result().error.empty());
        EXPECT_EQ(contents(folder.output()), "original-target"); EXPECT_TRUE(folder.noStaging());
    }
}

TEST(ExportEngineTest, CancelDuringRenderingPreservesOldTargetAndCleansStaging) {
    OutputFolder folder;
    { std::ofstream output(folder.output(), std::ios::binary); output << "keep-me"; }
    ExportEngine engine; ExportRequest request;
    request.snapshot = snapshot(72); request.outputPath = utf8(folder.output()); request.overwrite = true;
    engine.onProgress = [&](const ExportProgress& progress) {
        if (progress.state == ExportState::Rendering) engine.cancel();
    };
    ASSERT_TRUE(engine.start(request)); engine.wait();
    EXPECT_EQ(engine.result().state, ExportState::Cancelled);
    EXPECT_EQ(contents(folder.output()), "keep-me"); EXPECT_TRUE(folder.noStaging());
}

TEST(ExportEngineTest, RefusesInputMediaAsOutput) {
    ExportEngine engine; ExportRequest request;
    request.snapshot = snapshot(); request.outputPath = HEISENBERG_PLAYLIST_CLIP_A; request.overwrite = true;
    ASSERT_TRUE(engine.start(request)); engine.wait();
    EXPECT_EQ(engine.result().state, ExportState::Failed);
    EXPECT_NE(engine.result().error.find("input media"), std::string::npos);
}
