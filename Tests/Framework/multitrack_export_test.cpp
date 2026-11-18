#include <Compositing/CompositionBuilder.hpp>
#include <Compositing/CompositionExecutor.hpp>
#include <Export/ExportEngine.hpp>
#include <MultiMedia/Video/Renderer/Color/ColorPipeline.hpp>
#include <MultiMedia/Video/Renderer/CompositionRenderer.hpp>
#include <Platform/GpuContext.hpp>
#include <Platform/Vulkan/VulkanContext.hpp>
#include <Project/VideoProject.hpp>
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

namespace {
using namespace heisenberg;
using namespace heisenberg::renderer;
namespace fs = std::filesystem;
using Patches = std::array<double, 8>;
constexpr int kFrames = 12, kRate = 48000, kSamplesPerFrame = 2000;
constexpr int kSamples = kFrames * kSamplesPerFrame;

std::string utf8(const fs::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}
struct TemporaryProject {
    fs::path root = fs::temp_directory_path() / ("heisenberg-multitrack-export-" + makeUuid());
    TemporaryProject() { fs::create_directories(root); }
    ~TemporaryProject() { std::error_code ignored; fs::remove_all(root, ignored); }
};

// Identical interior strips in preview RGB and encoded Y avoid chroma edges.
Patches patches(const AVFrame& frame, bool rgba) {
    Patches values{};
    for (int p = 0; p < 8; ++p) {
        for (int y = 16; y < 104; ++y) for (int x = p * 20 + 5; x < p * 20 + 15; ++x) {
            if (rgba) {
                const auto* pixel = reinterpret_cast<const float*>(frame.data[0] + y * frame.linesize[0]) + x * 4;
                values[p] += 16 + 219 * (0.2126 * std::clamp(pixel[0], 0.0f, 1.0f)
                    + 0.7152 * std::clamp(pixel[1], 0.0f, 1.0f) + 0.0722 * std::clamp(pixel[2], 0.0f, 1.0f));
            } else values[p] += frame.data[0][y * frame.linesize[0] + x];
        }
        values[p] /= 880;
    }
    return values;
}
struct Readback {
    std::vector<Patches> pictures;
    std::vector<float> pcm;
    double audioEnd = 0;
};
Readback readback(const fs::path& path) {
    AVFormatContext* raw = nullptr;
    EXPECT_GE(avformat_open_input(&raw, utf8(path).c_str(), nullptr, nullptr), 0);
    if (!raw) return {};
    std::unique_ptr<AVFormatContext, void(*)(AVFormatContext*)> input(raw,
        [](AVFormatContext* value) { avformat_close_input(&value); });
    EXPECT_GE(avformat_find_stream_info(raw, nullptr), 0);
    const int indices[]{av_find_best_stream(raw, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0),
                        av_find_best_stream(raw, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0)};
    EXPECT_GE(indices[0], 0); EXPECT_GE(indices[1], 0);
    if (indices[0] < 0 || indices[1] < 0) return {};
    EXPECT_EQ(raw->streams[indices[0]]->codecpar->codec_id, AV_CODEC_ID_H264);
    EXPECT_EQ(raw->streams[indices[1]]->codecpar->codec_id, AV_CODEC_ID_AAC);
    EXPECT_EQ(raw->streams[indices[0]]->codecpar->color_space, AVCOL_SPC_BT709);
    EXPECT_NEAR(raw->streams[indices[1]]->duration * av_q2d(raw->streams[indices[1]]->time_base), 0.5, 1.0 / kRate);
    using Decoder = std::unique_ptr<AVCodecContext, void(*)(AVCodecContext*)>;
    const auto freeDecoder = [](AVCodecContext* value) { avcodec_free_context(&value); };
    std::array<Decoder, 2> decoders{Decoder(nullptr, freeDecoder), Decoder(nullptr, freeDecoder)};
    for (int i = 0; i < 2; ++i) {
        const auto* parameters = raw->streams[indices[i]]->codecpar;
        decoders[i].reset(avcodec_alloc_context3(avcodec_find_decoder(parameters->codec_id)));
        EXPECT_TRUE(decoders[i]);
        if (!decoders[i]) return {};
        if (avcodec_parameters_to_context(decoders[i].get(), parameters) < 0
            || avcodec_open2(decoders[i].get(), nullptr, nullptr) < 0) {
            ADD_FAILURE() << "Cannot open exported stream decoder";
            return {};
        }
    }
    Readback result;
    std::unique_ptr<AVFrame, void(*)(AVFrame*)> frame(av_frame_alloc(),
        [](AVFrame* value) { av_frame_free(&value); });
    std::unique_ptr<AVPacket, void(*)(AVPacket*)> packet(av_packet_alloc(),
        [](AVPacket* value) { av_packet_free(&value); });
    EXPECT_TRUE(frame); EXPECT_TRUE(packet);
    if (!frame || !packet) return {};
    const auto pull = [&](int index) {
        int status;
        while ((status = avcodec_receive_frame(decoders[index].get(), frame.get())) == 0) {
            if (index == 0) {
                EXPECT_EQ(frame->width, 160); EXPECT_EQ(frame->height, 120);
                EXPECT_EQ(frame->format, AV_PIX_FMT_YUV420P);
                if (frame->width == 160 && frame->height == 120 && frame->format == AV_PIX_FMT_YUV420P)
                    result.pictures.push_back(patches(*frame, false));
            } else {
                EXPECT_EQ(frame->sample_rate, kRate); EXPECT_EQ(frame->format, AV_SAMPLE_FMT_FLTP);
                if (frame->format == AV_SAMPLE_FMT_FLTP) {
                    const auto* samples = reinterpret_cast<const float*>(frame->data[0]);
                    result.pcm.insert(result.pcm.end(), samples, samples + frame->nb_samples);
                }
                if (frame->pts != AV_NOPTS_VALUE)
                    result.audioEnd = frame->pts * av_q2d(raw->streams[indices[1]]->time_base)
                        + static_cast<double>(frame->nb_samples) / kRate;
            }
            av_frame_unref(frame.get());
        }
        EXPECT_TRUE(status == AVERROR(EAGAIN) || status == AVERROR_EOF) << status;
    };
    int status;
    while ((status = av_read_frame(raw, packet.get())) >= 0) {
        for (int i = 0; i < 2; ++i) if (packet->stream_index == indices[i]) {
            EXPECT_GE(avcodec_send_packet(decoders[i].get(), packet.get()), 0); pull(i);
        }
        av_packet_unref(packet.get());
    }
    EXPECT_EQ(status, AVERROR_EOF);
    for (int i = 0; i < 2; ++i) { EXPECT_GE(avcodec_send_packet(decoders[i].get(), nullptr), 0); pull(i); }
    return result;
}

double tone(const std::vector<float>& pcm, int begin, int end) {
    if (static_cast<int>(pcm.size()) < end) return 0;
    double real = 0, imaginary = 0;
    for (int i = begin; i < end; ++i) {
        const double phase = 6.283185307179586 * 880 * i / kRate;
        real += pcm[i] * std::cos(phase); imaginary += pcm[i] * std::sin(phase);
    }
    return 2 * std::hypot(real, imaginary) / (end - begin);
}

std::shared_ptr<AVFrame> workingFrame(CompositionRenderer& renderer, const CompositionPlan& plan,
                                    int frame, std::string& error) {
    CompositionExecutor executor;
    const auto opened = executor.open(plan, {false, CompositionErrorPolicy::Strict});
    EXPECT_TRUE(opened) << opened.error;
    if (!opened) return {};
    const auto decoded = executor.videoFrame(frame);
    EXPECT_TRUE(decoded) << decoded.error;
    if (!decoded) return {};
    auto working = renderer.render(plan, frame, decoded.value.layers, &error);
    EXPECT_TRUE(working) << error;
    return working ? renderer.download(working, DownloadFormat::RgbaFloat, &error) : nullptr;
}
float pixel(const AVFrame& frame, int patch, int channel) {
    return reinterpret_cast<const float*>(frame.data[0] + 60 * frame.linesize[0])[(patch * 20 + 10) * 4 + channel];
}
} // namespace

TEST(MultitrackExportTest, TwoVideoThreeAudioTracksPersistCompositeMixAndExportThroughAudioTail) {
    TemporaryProject folder;
    VideoProject project;
    ProjectFile file;
    Timeline timeline;
    timeline.id = "multitrack"; timeline.width = 160; timeline.height = 120; timeline.fps = 24;
    const auto video = [](const char* id, const char* media, double opacity) {
        Track track; track.id = id; track.muted = true; // Suppress testsrc's embedded 440 Hz.
        Clip clip; clip.id = std::string(id) + "-clip"; clip.mediaRef = media;
        clip.durationFrames = 6; clip.opacity = opacity;
        track.clips.push_back(clip); return track;
    };
    timeline.tracks = {video("gray-top", "gray", 0.4), video("pattern-bottom", "pattern", 1.0)};
    for (int i = 0; i < 3; ++i) {
        Track track; track.id = "audio-" + std::to_string(i); track.type = ClipType::Audio;
        track.hidden = i == 1; // Hidden pictures must not suppress this audio contribution.
        Clip clip; clip.id = track.id + "-clip"; clip.mediaRef = "tone";
        clip.mediaType = clip.sourceClipType = ClipType::Audio;
        clip.startFrame = i == 2 ? 2 : 0; clip.durationFrames = kFrames - clip.startFrame;
        clip.trimStartFrame = i; clip.volume = i == 0 ? 0.3 : i == 1 ? 0.2 : 0.1;
        track.clips.push_back(clip); timeline.tracks.push_back(track);
    }
    file.timelines.push_back(timeline); file.activeTimelineId = timeline.id;
    file.colorSettings.inputColorSpaces["gray"] = color::kExportDisplay;
    file.colorSettings.inputColorSpaces["pattern"] = color::kExportDisplay;
    MediaManifest manifest;
    const auto addMedia = [&](const char* id, const char* path, ClipType type, double duration, bool hasAudio) {
        MediaManifestEntry entry; entry.id = id; entry.name = id; entry.type = type;
        entry.duration = duration; entry.hasAudio = hasAudio;
        entry.source = {MediaSource::Kind::External, path};
        EXPECT_TRUE(manifest.add(entry));
    };
    addMedia("gray", HEISENBERG_TEST_SDR_GRAY, ClipType::Video, 0.25, false);
    addMedia("pattern", HEISENBERG_PLAYLIST_CLIP_A, ClipType::Video, 1, true);
    addMedia("tone", HEISENBERG_TEST_LONG_AUDIO, ClipType::Audio, 3, true);
    std::string error;
    ASSERT_TRUE(project.editor().replaceDocument(file, manifest, {}, &error)) << error;
    CompositionPlan before;
    ASSERT_TRUE(CompositionBuilder::build(project.snapshot(), before, &error)) << error;
    ASSERT_TRUE(project.saveAs(utf8(folder.root / "multitrack.berg"), &error)) << error;
    VideoProject reopened;
    ASSERT_TRUE(reopened.loadPackage(utf8(folder.root / "multitrack.berg"), &error)) << error;
    const auto snapshot = reopened.snapshot();
    CompositionPlan plan;
    ASSERT_TRUE(CompositionBuilder::build(snapshot, plan, &error)) << error;
    ASSERT_EQ(plan.tracks.size(), 5u); ASSERT_EQ(before.tracks.size(), plan.tracks.size());
    EXPECT_EQ(plan.frameCount, kFrames); EXPECT_EQ(plan.frameCount, before.frameCount);
    EXPECT_EQ(plan.settings, before.settings); EXPECT_EQ(plan.colorSettings, before.colorSettings);
    EXPECT_EQ(plan.warnings, before.warnings);
    for (size_t i = 0; i < plan.tracks.size(); ++i) {
        const auto& actual = plan.tracks[i]; const auto& expected = before.tracks[i];
        EXPECT_EQ(actual.id, expected.id); EXPECT_EQ(actual.type, expected.type);
        EXPECT_EQ(actual.hidden, expected.hidden); EXPECT_EQ(actual.muted, expected.muted);
        ASSERT_EQ(actual.clips.size(), 1u); ASSERT_EQ(expected.clips.size(), 1u);
        const auto& a = actual.clips.front(); const auto& b = expected.clips.front();
        EXPECT_EQ(a.visualSnapshot, b.visualSnapshot); EXPECT_EQ(a.id, b.id);
        EXPECT_EQ(a.mediaRef, b.mediaRef); EXPECT_EQ(a.sourcePath, b.sourcePath);
        EXPECT_EQ(a.startFrame, b.startFrame); EXPECT_EQ(a.durationFrames, b.durationFrames);
        EXPECT_EQ(a.trimStartFrame, b.trimStartFrame); EXPECT_EQ(a.trimEndFrame, b.trimEndFrame);
        EXPECT_DOUBLE_EQ(a.volume, b.volume); EXPECT_DOUBLE_EQ(a.speed, b.speed);
    }
    EXPECT_EQ(plan.tracks[0].type, ClipType::Video); EXPECT_EQ(plan.tracks[1].type, ClipType::Video);
    for (int i = 2; i < 5; ++i) EXPECT_EQ(plan.tracks[i].type, ClipType::Audio);
    EXPECT_TRUE(plan.tracks[3].hidden); EXPECT_FALSE(plan.tracks[3].muted);

    // Decode one unit-gain source, then independently index source PCM by the
    // fixture's explicit trims/starts. Do not derive this oracle from the mixed plan.
    auto single = plan;
    single.tracks = {plan.tracks[2]}; single.frameCount = 14;
    single.tracks[0].clips[0].durationFrames = 14;
    single.tracks[0].clips[0].visualSnapshot.durationFrames = 14;
    single.tracks[0].clips[0].volume = 1;
    single.tracks[0].clips[0].visualSnapshot.volume = 1;
    CompositionExecutor reference;
    ASSERT_TRUE(reference.open(single, {false, CompositionErrorPolicy::Strict}));
    const auto unit = reference.audioSamples(0, 14 * kSamplesPerFrame);
    ASSERT_TRUE(unit) << unit.error; ASSERT_TRUE(unit.value);
    ASSERT_EQ(unit.value->samples(), 14 * kSamplesPerFrame);
    ASSERT_EQ(unit.value->spec().channels, 2);
    ASSERT_EQ(unit.value->spec().sampleRate, kRate);
    CompositionExecutor executor;
    const auto opened = executor.open(plan, {false, CompositionErrorPolicy::Strict});
    ASSERT_TRUE(opened) << opened.error;
    EXPECT_EQ(opened.value.audioSampleCount, kSamples); EXPECT_DOUBLE_EQ(opened.value.duration, 0.5);
    const auto mixed = executor.audioSamples(0, kSamples);
    ASSERT_TRUE(mixed) << mixed.error; ASSERT_TRUE(mixed.value);
    ASSERT_EQ(mixed.value->samples(), kSamples); ASSERT_EQ(mixed.value->spec().channels, 2);
    std::vector<float> expectedPcm(kSamples);
    double mixError = 0, signalEnergy = 0;
    for (int ch = 0; ch < 2; ++ch) for (int s = 0; s < kSamples; ++s) {
        const double expected = 0.3 * unit.value->sampleRo(ch, s)
            + 0.2 * unit.value->sampleRo(ch, s + kSamplesPerFrame)
            + (s >= 2 * kSamplesPerFrame ? 0.1 * unit.value->sampleRo(ch, s) : 0);
        mixError = std::max(mixError, std::abs(mixed.value->sampleRo(ch, s) - expected));
        signalEnergy += expected * expected;
        if (ch == 0) expectedPcm[s] = static_cast<float>(expected);
    }
    EXPECT_LT(mixError, 2e-6); EXPECT_GT(std::sqrt(signalEnergy / (2 * kSamples)), 0.01);
    auto zeroOpacityPcm = plan; zeroOpacityPcm.tracks[3].clips[0].visualSnapshot.opacity = 0;
    CompositionExecutor zeroOpacityExecutor;
    ASSERT_TRUE(zeroOpacityExecutor.open(zeroOpacityPcm, {false, CompositionErrorPolicy::Strict}));
    const auto unchangedAudio = zeroOpacityExecutor.audioSamples(0, kSamples);
    ASSERT_TRUE(unchangedAudio) << unchangedAudio.error; ASSERT_TRUE(unchangedAudio.value);
    ASSERT_EQ(unchangedAudio.value->samples(), kSamples);
    double opacityAudioError = 0;
    for (int s = 0; s < kSamples; ++s)
        opacityAudioError = std::max(opacityAudioError,
            static_cast<double>(std::abs(unchangedAudio.value->sampleRo(0, s) - mixed.value->sampleRo(0, s))));
    EXPECT_LT(opacityAudioError, 2e-6);
    auto muted = plan; muted.tracks[3].muted = true;
    CompositionExecutor mutedExecutor;
    ASSERT_TRUE(mutedExecutor.open(muted, {false, CompositionErrorPolicy::Strict}));
    const auto mutedPcm = mutedExecutor.audioSamples(0, kSamples);
    ASSERT_TRUE(mutedPcm) << mutedPcm.error; ASSERT_TRUE(mutedPcm.value);
    ASSERT_EQ(mutedPcm.value->samples(), kSamples);
    double muteError = 0, removedEnergy = 0;
    for (int s = 0; s < kSamples; ++s) {
        const double removed = 0.2 * unit.value->sampleRo(0, s + kSamplesPerFrame);
        muteError = std::max(muteError, std::abs(mixed.value->sampleRo(0, s) - mutedPcm.value->sampleRo(0, s) - removed));
        removedEnergy += removed * removed;
    }
    EXPECT_LT(muteError, 2e-6); EXPECT_GT(std::sqrt(removedEnergy / kSamples), 0.01);

    auto& vk = VulkanContext::instance();
    vk.initLoader(); vk.createInstance(false); vk.createDevice();
    VulkanResources resources{vk.vkInstance(), vk.physicalDevice(), vk.device(),
        vk.graphicsQueueFamily(), vk.graphicsQueue(), vk.getInstanceProcAddr()};
    resources.enabledFeatures = vk.enabledDeviceFeatures(); resources.enabledExtensions = vk.enabledDeviceExtensions();
    GpuContext gpu(resources);
    CompositionRenderer renderer;
    ASSERT_TRUE(renderer.initialize(gpu.plGpu(), gpu.plVulkan(), nullptr, nullptr, &error)) << error;
    auto composite = workingFrame(renderer, plan, 2, error); ASSERT_TRUE(composite) << error;
    auto grayOnly = plan; grayOnly.tracks[1].hidden = true; grayOnly.tracks[0].clips[0].visualSnapshot.opacity = 1;
    auto gray = workingFrame(renderer, grayOnly, 2, error); ASSERT_TRUE(gray) << error;
    auto hidden = plan; hidden.tracks[0].hidden = true;
    auto pattern = workingFrame(renderer, hidden, 2, error); ASSERT_TRUE(pattern) << error;
    auto reordered = plan; std::swap(reordered.tracks[0], reordered.tracks[1]);
    auto reorderedPixels = workingFrame(renderer, reordered, 2, error); ASSERT_TRUE(reorderedPixels) << error;
    auto transparent = plan; transparent.tracks[0].clips[0].visualSnapshot.opacity = 0;
    auto transparentPixels = workingFrame(renderer, transparent, 2, error); ASSERT_TRUE(transparentPixels) << error;
    auto unmutedVideo = plan; unmutedVideo.tracks[0].muted = unmutedVideo.tracks[1].muted = false;
    auto unmutedPixels = workingFrame(renderer, unmutedVideo, 2, error); ASSERT_TRUE(unmutedPixels) << error;
    double reorderDifference = 0;
    for (int p = 0; p < 8; ++p) for (int c = 0; c < 3; ++c) {
        const float bottom = pixel(*pattern, p, c), blended = pixel(*composite, p, c);
        EXPECT_NEAR(blended, 0.4 * pixel(*gray, p, c) + 0.6 * bottom, 0.003) << p << ":" << c;
        EXPECT_NEAR(pixel(*reorderedPixels, p, c), bottom, 0.001);
        EXPECT_NEAR(pixel(*transparentPixels, p, c), bottom, 0.001);
        EXPECT_NEAR(pixel(*unmutedPixels, p, c), blended, 0.001);
        reorderDifference = std::max(reorderDifference, static_cast<double>(std::abs(blended - bottom)));
    }
    EXPECT_GT(reorderDifference, 0.02);

    std::vector<Patches> preview;
    ColorOutputRequest output;
    output.display = plan.colorSettings.exportOutput.display; output.view = plan.colorSettings.exportOutput.view;
    for (int frame = 0; frame < kFrames; ++frame) {
        const auto decoded = executor.videoFrame(frame);
        ASSERT_TRUE(decoded) << decoded.error;
        ASSERT_EQ(decoded.value.layers.size(), frame < 6 ? 2u : 0u);
        EXPECT_EQ(decoded.value.blank, frame >= 6);
        if (frame < 6) {
            EXPECT_EQ(decoded.value.layers[0].trackId, "gray-top");
            EXPECT_EQ(decoded.value.layers[1].trackId, "pattern-bottom");
            EXPECT_DOUBLE_EQ(decoded.value.layers[0].opacity, 0.4);
        }
        auto working = renderer.render(plan, frame, decoded.value.layers, &error);
        ASSERT_TRUE(working) << error;
        auto encoded = renderer.output(working, output, &error); ASSERT_TRUE(encoded) << error;
        auto pixels = renderer.download(encoded, DownloadFormat::RgbaFloat, &error); ASSERT_TRUE(pixels) << error;
        preview.push_back(patches(*pixels, true));
    }
    renderer.shutdown();
    ExportRequest request; request.snapshot = snapshot; request.outputPath = utf8(folder.root / "multitrack.mp4");
    request.hardwareDecode = false; request.videoCodec = ExportVideoCodec::H264; request.crf = 0;
    ExportEngine engine;
    ASSERT_TRUE(engine.start(request, &error)) << error; engine.wait();
    ASSERT_TRUE(engine.result().succeeded()) << engine.result().stage << ": " << engine.result().error;
    EXPECT_EQ(engine.progress().totalFrames, kFrames); EXPECT_EQ(engine.progress().completedFrames, kFrames);
    EXPECT_EQ(engine.progress().totalSamples, kSamples); EXPECT_EQ(engine.progress().completedSamples, kSamples);
    const auto actual = readback(folder.root / "multitrack.mp4");
    ASSERT_EQ(actual.pictures.size(), static_cast<size_t>(kFrames));
    for (int f = 0; f < kFrames; ++f) for (int p = 0; p < 8; ++p) {
        EXPECT_NEAR(actual.pictures[f][p], preview[f][p], 2.0) << "frame " << f << " patch " << p;
        if (f >= 6) EXPECT_NEAR(actual.pictures[f][p], 16, 1.0); // Audio-only tail renders black.
    }
    ASSERT_GE(actual.pcm.size(), static_cast<size_t>(kSamples));
    EXPECT_LE(actual.pcm.size(), static_cast<size_t>(kSamples + 1023));
    EXPECT_NEAR(actual.audioEnd, 0.5, 1024.0 / kRate);
    for (const auto range : {std::array{4800, 9600}, std::array{14400, 21600}}) {
        const auto expected = tone(expectedPcm, range[0], range[1]);
        EXPECT_GT(expected, 0.02);
        EXPECT_NEAR(tone(actual.pcm, range[0], range[1]), expected, 0.006);
        double squaredError = 0;
        for (int s = range[0]; s < range[1]; ++s) {
            const double difference = actual.pcm[s] - expectedPcm[s];
            squaredError += difference * difference;
        }
        // Compare phase as well as tone amplitude: dropping a trimmed source
        // can leave a similar 880 Hz magnitude while changing the mixed waveform.
        EXPECT_LT(std::sqrt(squaredError / (range[1] - range[0])), 0.004);
    }
    for (const auto& entry : fs::directory_iterator(folder.root)) EXPECT_NE(entry.path().extension(), ".partial");
}
