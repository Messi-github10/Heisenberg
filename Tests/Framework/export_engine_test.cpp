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
