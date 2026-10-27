#include <Compositing/CompositionBuilder.hpp>
#include <Export/ExportEngine.hpp>
#include <Project/VideoProject.hpp>
#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
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
        ASSERT_EQ(preview.clips.size(), expected.size());
        EXPECT_TRUE(preview.audioClips.empty()); // Export still carries embedded video audio.
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
            const auto& planned = preview.clips[index];
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
