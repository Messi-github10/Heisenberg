#include <Common/AudioFrame.hpp>
#include <MultiMedia/Audio/Encoder/AudioEncoder.hpp>
#include <MultiMedia/Muxer/Muxer.hpp>
#include <MultiMedia/Video/Encoder/EncoderInternal.hpp>
#include <MultiMedia/Video/Encoder/VideoEncoder.hpp>
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <numeric>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/intreadwrite.h>
}

using namespace heisenberg;
namespace {
using namespace heisenberg::encoding_detail;
void append(EncodedPackets& destination, EncodedPackets source) {
    for (auto& packet : source) destination.push_back(std::move(packet));
}
struct TempOutput {
    std::filesystem::path directory;
    TempOutput() {
        static std::atomic<unsigned> sequence{0};
        directory = std::filesystem::temp_directory_path() /
            ("heisenberg_encoding_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
             "_" + std::to_string(sequence++));
        std::filesystem::create_directories(directory);
    }
    ~TempOutput() { std::error_code error; std::filesystem::remove_all(directory, error); }
    std::string path() const { return (directory / "encoded.mp4").string(); }
};
struct InputDeleter { void operator()(AVFormatContext* p) const { avformat_close_input(&p); } };
using InputPtr = std::unique_ptr<AVFormatContext, InputDeleter>;
InputPtr inputFile(const std::string& path) {
    AVFormatContext* raw = nullptr;
    const int result = avformat_open_input(&raw, path.c_str(), nullptr, nullptr);
    InputPtr input(raw);
    check(result, "open encoded test file");
    check(avformat_find_stream_info(input.get(), nullptr), "read encoded test stream info");
    return input;
}
class Decode {
public:
    CodecPtr context;
    FramePtr decoded = frame();
    int frames = 0;
    int64_t samples = 0;
    explicit Decode(const AVCodecParameters* parameters, AVRational timebase) {
        const auto* codec = avcodec_find_decoder(parameters->codec_id);
        if (!codec) throw EncodingError("test decoder unavailable");
        context.reset(avcodec_alloc_context3(codec));
        if (!context) throw std::bad_alloc();
        check(avcodec_parameters_to_context(context.get(), parameters), "test decoder parameters");
        context->pkt_timebase = timebase;
        check(avcodec_open2(context.get(), codec, nullptr), "open test decoder");
    }
    template<class Consumer> void push(const AVPacket* packet, Consumer consume) {
        auto receive = [&]() {
            for (;;) {
                const int result = avcodec_receive_frame(context.get(), decoded.get());
                if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return result;
                check(result, "decode test frame");
                ++frames;
                samples += decoded->nb_samples;
                consume(*decoded);
                av_frame_unref(decoded.get());
            }
        };
        int result = avcodec_send_packet(context.get(), packet);
        if (result == AVERROR(EAGAIN)) { receive(); result = avcodec_send_packet(context.get(), packet); }
        check(result, "send test decoder packet");
        const int end = receive();
        if (!packet && end != AVERROR_EOF) throw EncodingError("test decoder did not drain to EOF");
    }
};

// Four solid regions, with padded rows to catch accidental tight-stride reads.
FramePtr colorFrame(AVPixelFormat format) {
    auto rgb = frame();
    rgb->format = format; rgb->width = 64; rgb->height = 64;
    check(av_frame_get_buffer(rgb.get(), 32), "allocate RGB test frame");
    for (int y = 0; y < rgb->height; ++y) for (int x = 0; x < rgb->width; ++x) {
        const int quadrant = (y >= 32 ? 2 : 0) + (x >= 32 ? 1 : 0);
        const float values[4][4] = {{0, 0, 0, 1}, {1, 1, 1, 1}, {1, 0, 0, 1}, {0, 0, 1, 1}};
        const uint16_t halves[4][4] = {{0, 0, 0, 0x3c00}, {0x3c00, 0x3c00, 0x3c00, 0x3c00},
                                     {0x3c00, 0, 0, 0x3c00}, {0, 0, 0x3c00, 0x3c00}};
        auto* p = rgb->data[0] + y * rgb->linesize[0];
        if (format == AV_PIX_FMT_RGBAF32) std::memcpy(p + x * 16, values[quadrant], 16);
        else if (format == AV_PIX_FMT_RGBAF16) std::memcpy(p + x * 8, halves[quadrant], 8);
        else for (int c = 0; c < 4; ++c) p[x * 4 + c] = static_cast<uint8_t>(values[quadrant][c] * 255);
    }
    return rgb;
}
void checkColor(const AVFrame& decoded) {
    ASSERT_EQ(decoded.format, AV_PIX_FMT_YUV420P);
    EXPECT_EQ(decoded.color_range, AVCOL_RANGE_MPEG);
    EXPECT_EQ(decoded.colorspace, AVCOL_SPC_BT709);
    EXPECT_EQ(decoded.color_primaries, AVCOL_PRI_BT709);
    EXPECT_EQ(decoded.color_trc, AVCOL_TRC_BT709);
    const int expectedY[] = {16, 235, 63, 32};
    const int expectedU[] = {128, 128, 102, 240};
    const int expectedV[] = {128, 128, 240, 118};
    for (int q = 0; q < 4; ++q) {
        const int x = (q & 1) ? 48 : 16, y = (q & 2) ? 48 : 16;
        EXPECT_NEAR(decoded.data[0][y * decoded.linesize[0] + x], expectedY[q], 3);
        EXPECT_NEAR(decoded.data[1][(y / 2) * decoded.linesize[1] + x / 2], expectedU[q], 3);
        EXPECT_NEAR(decoded.data[2][(y / 2) * decoded.linesize[2] + x / 2], expectedV[q], 3);
    }
}
EncodedPackets audioPackets(AudioEncoder& encoder, int totalSamples) {
    EncodedPackets result;
    for (int position = 0; position < totalSamples;) {
        const int count = std::min(totalSamples - position, position % 7 == 0 ? 317 : 1301);
        AudioFrame pcm;
        pcm.setPosition(position); pcm.setSamples(count, true);
        for (int c = 0; c < 2; ++c) for (int s = 0; s < count; ++s)
            pcm.sampleRw(c, s) = static_cast<float>(0.2 * std::sin((position + s) * (c == 0 ? 440.0 : 880.0) *
                                                                  6.283185307179586 / 48000.0));
        append(result, encoder.encode(pcm));
        position += count;
    }
    append(result, encoder.drain());
    return result;
}
}

TEST(EncodingTest, ParametersAndPacketsDeepCopyAllMetadata) {
    AVCodecParameters* raw = avcodec_parameters_alloc();
    ASSERT_NE(raw, nullptr);
    raw->codec_type = AVMEDIA_TYPE_AUDIO; raw->codec_id = AV_CODEC_ID_AAC;
    raw->sample_rate = 48000; raw->initial_padding = 1024; raw->trailing_padding = 523;
    av_channel_layout_default(&raw->ch_layout, 2);
    raw->extradata_size = 3;
    raw->extradata = static_cast<uint8_t*>(av_mallocz(3 + AV_INPUT_BUFFER_PADDING_SIZE));
    ASSERT_NE(raw->extradata, nullptr);
    std::memcpy(raw->extradata, "abc", 3);
    auto* coded = av_packet_side_data_new(&raw->coded_side_data, &raw->nb_coded_side_data,
                                          AV_PKT_DATA_SKIP_SAMPLES, 10, 0);
    ASSERT_NE(coded, nullptr);
    std::memset(coded->data, 0, coded->size); AV_WL32(coded->data, 1024);
    CodecParameters copy(*raw), another(copy);
    EXPECT_NE(copy.native()->coded_side_data, raw->coded_side_data);
    EXPECT_NE(copy.native()->coded_side_data[0].data, raw->coded_side_data[0].data);
    EXPECT_NE(copy.native()->extradata, raw->extradata);
    avcodec_parameters_free(&raw);
    EXPECT_EQ(another.native()->initial_padding, 1024);
    EXPECT_EQ(another.native()->trailing_padding, 523);
    EXPECT_EQ(another.native()->ch_layout.nb_channels, 2);
    ASSERT_EQ(another.native()->nb_coded_side_data, 1);
    EXPECT_EQ(another.native()->coded_side_data[0].type, AV_PKT_DATA_SKIP_SAMPLES);
    EXPECT_EQ(AV_RL32(another.native()->coded_side_data[0].data), 1024);
    EXPECT_EQ(std::memcmp(another.native()->extradata, "abc", 3), 0);
    auto packet = encoding_detail::packet();
    check(av_new_packet(packet.get(), 4), "allocate test packet");
    packet->pts = -1024; packet->dts = -1024; packet->duration = 1024;
    auto* data = av_packet_new_side_data(packet.get(), AV_PKT_DATA_SKIP_SAMPLES, 10);
    ASSERT_NE(data, nullptr);
    std::memset(data, 0, 10); AV_WL32(data, 1024); AV_WL32(data + 4, 523);
    EncodedPacket encoded(*packet, {1, 48000});
    av_packet_unref(packet.get());
    EXPECT_EQ(encoded.native()->pts, -1024);
    size_t size = 0;
    const auto* side = av_packet_get_side_data(encoded.native(), AV_PKT_DATA_SKIP_SAMPLES, &size);
    ASSERT_NE(side, nullptr); ASSERT_EQ(size, 10u);
    EXPECT_EQ(AV_RL32(side), 1024u); EXPECT_EQ(AV_RL32(side + 4), 523u);
}

TEST(EncodingTest, FloatAndHalfRgbPerformRealLimitedBt709Conversion) {
    for (const auto format : {AV_PIX_FMT_RGBAF32, AV_PIX_FMT_RGBAF16, AV_PIX_FMT_RGBA}) {
        SCOPED_TRACE(format);
        VideoEncoderSettings settings; settings.width = 64; settings.height = 64; settings.crf = 0;
        VideoEncoder encoder(settings);
        auto rgb = colorFrame(format);
        auto packets = encoder.encode(*rgb, 0); append(packets, encoder.drain());
        auto parameters = encoder.codecParameters();
        Decode decoder(parameters.native(), encoder.timeBase());
        for (const auto& packet : packets) decoder.push(packet.native(), checkColor);
        decoder.push(nullptr, checkColor);
        EXPECT_EQ(decoder.frames, 1);
        EXPECT_TRUE(encoder.drain().empty());
        EXPECT_THROW(encoder.encode(*rgb, 1), EncodingError);
    }
}

TEST(EncodingTest, RejectsOddDimensionsInvalidFloatStrideAndTimeline) {
    VideoEncoderSettings settings; settings.width = 63; settings.height = 64;
    EXPECT_THROW(VideoEncoder odd(settings), std::invalid_argument);
    settings.width = 64; settings.frameRate = {0, 1};
    EXPECT_THROW(VideoEncoder invalidRate(settings), std::invalid_argument);
    settings.frameRate = {24, 1};
    VideoEncoder encoder(settings);
    auto rgb = colorFrame(AV_PIX_FMT_RGBAF32);
    float invalid = std::numeric_limits<float>::quiet_NaN();
    std::memcpy(rgb->data[0], &invalid, sizeof(invalid));
    EXPECT_THROW(encoder.encode(*rgb, 0), std::invalid_argument);
    rgb = colorFrame(AV_PIX_FMT_RGBAF32);
    EXPECT_THROW(encoder.encode({rgb->data[0], 1, 64, 64, EncodedRgbFormat::RGBA32F}, 0), std::invalid_argument);
    encoder.encode(*rgb, 0);
    EXPECT_THROW(encoder.encode(*rgb, 0), std::invalid_argument);
    encoder.drain();
}

TEST(EncodingTest, AacFifoRetainsExactDurationPrimingTailAndGaplessDecode) {
    for (const int samples : {1, 100, 1024, 2501, 48001}) {
        SCOPED_TRACE(samples);
        AudioEncoder encoder;
        auto packets = audioPackets(encoder, samples);
        ASSERT_FALSE(packets.empty());
        EXPECT_EQ(encoder.samplesSubmitted(), samples);
        const auto* first = packets.front().native();
        EXPECT_EQ(first->pts, -encoder.initialPadding());
        EXPECT_EQ(first->dts, first->pts);
        size_t size = 0;
        const auto* initial = av_packet_get_side_data(first, AV_PKT_DATA_SKIP_SAMPLES, &size);
        ASSERT_NE(initial, nullptr); ASSERT_EQ(size, 10u);
        EXPECT_EQ(AV_RL32(initial), static_cast<unsigned>(encoder.initialPadding()));
        const auto* last = packets.back().native();
        const auto* tail = av_packet_get_side_data(last, AV_PKT_DATA_SKIP_SAMPLES, &size);
        ASSERT_NE(tail, nullptr); ASSERT_EQ(size, 10u);
        EXPECT_EQ(last->pts + last->duration, samples);
        EXPECT_EQ(AV_RL32(tail + 4), static_cast<unsigned>(encoder.frameSize() - last->duration));
        const auto parameters = encoder.codecParameters();
        EXPECT_EQ(parameters.native()->trailing_padding, static_cast<int>(AV_RL32(tail + 4)));
        Decode decoder(parameters.native(), encoder.timeBase());
        double channelEnergy[2]{};
        auto consume = [&](const AVFrame& pcm) {
            ASSERT_EQ(pcm.format, AV_SAMPLE_FMT_FLTP);
            EXPECT_EQ(pcm.sample_rate, 48000); EXPECT_EQ(pcm.ch_layout.nb_channels, 2);
            for (int c = 0; c < 2; ++c) for (int s = 0; s < pcm.nb_samples; ++s) {
                const auto value = reinterpret_cast<const float*>(pcm.extended_data[c])[s];
                EXPECT_TRUE(std::isfinite(value)); channelEnergy[c] += value * value;
            }
        };
        for (const auto& packet : packets) decoder.push(packet.native(), consume);
        decoder.push(nullptr, consume);
        EXPECT_EQ(decoder.samples, samples); // strict, not rounded to AAC frame size
        if (samples > 1024) { EXPECT_GT(channelEnergy[0], 1.0); EXPECT_GT(channelEnergy[1], 1.0); }
        EXPECT_TRUE(encoder.drain().empty());
        float silence = 0;
        EXPECT_THROW(encoder.encode(&silence, &silence, 1), EncodingError);
    }
}

TEST(EncodingTest, AacRejectsDiscontinuousOrWrongFormatInputAndHandlesEmptyDrain) {
    AudioEncoder encoder;
    AudioFrame pcm; pcm.setPosition(1); pcm.setSamples(10, true);
    EXPECT_THROW(encoder.encode(pcm), std::invalid_argument);
    AudioSpec spec; spec.sampleRate = 44100; pcm.setSpec(spec); pcm.setPosition(0);
    EXPECT_THROW(encoder.encode(pcm), std::invalid_argument);
    float invalid = std::numeric_limits<float>::infinity(), silence = 0;
    EXPECT_THROW(encoder.encode(&invalid, &silence, 1), std::invalid_argument);
    EXPECT_TRUE(encoder.drain().empty());
}

TEST(EncodingTest, PlanarAvFrameAudioInputUsesTheSameFifoAndSampleTimebase) {
    AudioEncoder encoder;
    auto pcm = frame();
    pcm->format = AV_SAMPLE_FMT_FLTP; pcm->sample_rate = 48000;
    av_channel_layout_default(&pcm->ch_layout, 2);
    pcm->nb_samples = 2501; pcm->pts = 0; pcm->time_base = {1, 48000};
    check(av_frame_get_buffer(pcm.get(), 32), "allocate planar PCM test frame");
    for (int c = 0; c < 2; ++c)
        std::fill_n(reinterpret_cast<float*>(pcm->extended_data[c]), pcm->nb_samples, 0.1f);
    auto packets = encoder.encode(*pcm);
    append(packets, encoder.drain());
    EXPECT_EQ(encoder.samplesSubmitted(), 2501);
    Decode decoder(encoder.codecParameters().native(), encoder.timeBase());
    for (const auto& packet : packets) decoder.push(packet.native(), [](const AVFrame&) {});
    decoder.push(nullptr, [](const AVFrame&) {});
    EXPECT_EQ(decoder.samples, 2501);
}

TEST(EncodingTest, H264AndH265Mp4RoundTripPreserveBframesTagsAndRealAudioDuration) {
    for (const auto codec : {VideoEncodingCodec::H264, VideoEncodingCodec::H265}) {
        SCOPED_TRACE(static_cast<int>(codec));
        TempOutput output;
        VideoEncoderSettings settings;
        settings.width = 64; settings.height = 64; settings.codec = codec;
        settings.frameRate = {30000, 1001}; settings.sampleAspect = {4, 3};
        VideoEncoder video(settings); AudioEncoder audio;
        const auto originalVideoTimebase = video.timeBase();
        EXPECT_EQ(originalVideoTimebase.num, 1001); EXPECT_EQ(originalVideoTimebase.den, 30000);
        auto rgb = colorFrame(AV_PIX_FMT_RGBAF32);
        EncodedPackets videoPackets;
        for (int i = 0; i < 30; ++i) append(videoPackets, video.encode(*rgb, i));
        append(videoPackets, video.drain());
        ASSERT_EQ(videoPackets.size(), 30u);
        bool reorders = false;
        for (const auto& p : videoPackets) reorders |= p.native()->pts != p.native()->dts;
        EXPECT_TRUE(reorders); EXPECT_LT(videoPackets.front().native()->dts, 0);
        auto audioOutput = audioPackets(audio, 48001);
        Muxer muxer;
        const int vs = muxer.addStream(video.codecParameters(), video.timeBase());
        const int as = muxer.addStream(audio.codecParameters(), audio.timeBase());
        muxer.open(output.path());
        // Intentional changed stream timebase: tests post-header rescale.
        EXPECT_NE(av_cmp_q(muxer.streamTimeBase(vs), video.timeBase()), 0);
        size_t v = 0, a = 0;
        while (v < videoPackets.size() || a < audioOutput.size()) {
            const bool useVideo = a == audioOutput.size() || (v < videoPackets.size() &&
                av_compare_ts(videoPackets[v].native()->dts, video.timeBase(), audioOutput[a].native()->dts, audio.timeBase()) <= 0);
            if (useVideo) muxer.write(videoPackets[v++], vs); else muxer.write(audioOutput[a++], as);
        }
        muxer.setStreamDuration(vs, 30, video.timeBase());
        muxer.setStreamDuration(as, 48001, audio.timeBase());
        muxer.finish(); muxer.finish();
        EXPECT_THROW(muxer.write(videoPackets.front(), vs), EncodingError);
        // Muxing didn't mutate/consume caller packet or negative timestamp.
        EXPECT_LT(videoPackets.front().native()->dts, 0);
        auto input = inputFile(output.path());
        ASSERT_EQ(input->nb_streams, 2u);
        auto* videoStream = input->streams[vs]; auto* audioStream = input->streams[as];
        EXPECT_EQ(videoStream->codecpar->codec_id, codec == VideoEncodingCodec::H264 ? AV_CODEC_ID_H264 : AV_CODEC_ID_HEVC);
        EXPECT_EQ(videoStream->codecpar->width, 64); EXPECT_EQ(videoStream->codecpar->height, 64);
        EXPECT_GT(videoStream->codecpar->extradata_size, 0);
        EXPECT_EQ(av_cmp_q(videoStream->codecpar->sample_aspect_ratio, settings.sampleAspect), 0);
        if (codec == VideoEncodingCodec::H265) EXPECT_EQ(videoStream->codecpar->codec_tag, MKTAG('h','v','c','1'));
        EXPECT_EQ(audioStream->codecpar->sample_rate, 48000); EXPECT_EQ(audioStream->codecpar->ch_layout.nb_channels, 2);
        EXPECT_EQ(av_rescale_q(audioStream->duration, audioStream->time_base, audio.timeBase()), 48001);
        EXPECT_EQ(av_rescale_q(videoStream->duration, videoStream->time_base, video.timeBase()), 30);
        Decode vd(videoStream->codecpar, videoStream->time_base), ad(audioStream->codecpar, audioStream->time_base);
        auto packet = encoding_detail::packet();
        int64_t lastDts[2] = {AV_NOPTS_VALUE, AV_NOPTS_VALUE};
        bool sawInitialSkip = false;
        int64_t lastAudioEnd = 0;
        for (;;) {
            const int result = av_read_frame(input.get(), packet.get());
            if (result == AVERROR_EOF) break;
            check(result, "read encoded test packet");
            const int stream = packet->stream_index;
            ASSERT_GE(stream, 0); ASSERT_LT(stream, 2);
            if (lastDts[stream] != AV_NOPTS_VALUE) EXPECT_GT(packet->dts, lastDts[stream]);
            lastDts[stream] = packet->dts;
            if (stream == vs) vd.push(packet.get(), checkColor);
            else {
                size_t size = 0;
                if (const auto* skip = av_packet_get_side_data(packet.get(), AV_PKT_DATA_SKIP_SAMPLES, &size); skip && size >= 10) {
                    if (AV_RL32(skip)) { sawInitialSkip = true; EXPECT_LT(packet->pts, 0); }
                }
                lastAudioEnd = packet->pts + packet->duration;
                ad.push(packet.get(), [](const AVFrame&) {});
            }
            av_packet_unref(packet.get());
        }
        vd.push(nullptr, checkColor); ad.push(nullptr, [](const AVFrame&) {});
        EXPECT_EQ(vd.frames, 30); EXPECT_TRUE(sawInitialSkip);
        EXPECT_EQ(av_rescale_q(lastAudioEnd, audioStream->time_base, audio.timeBase()), 48001);
        // MOV represents the tail with sample duration/edit-list. libavcodec
        // alone need not reconstruct end skip from MP4 packet duration. A
        // consumer uses the exact stream duration above to trim presentation.
        EXPECT_GE(ad.samples, 48001); EXPECT_LT(ad.samples - 48001, audio.frameSize());
        RecordProperty(codec == VideoEncodingCodec::H264 ? "h264_mp4_decoded_audio_samples" : "h265_mp4_decoded_audio_samples",
                       std::to_string(ad.samples));
    }
}

TEST(EncodingTest, MuxerReportsOpenStateAndIoErrors) {
    VideoEncoderSettings settings; settings.width = 64; settings.height = 64;
    VideoEncoder encoder(settings);
    Muxer muxer;
    EXPECT_THROW(muxer.finish(), EncodingError);
    const int stream = muxer.addStream(encoder.codecParameters(), encoder.timeBase());
    EXPECT_THROW(muxer.streamTimeBase(10), std::invalid_argument);
    TempOutput output;
    EXPECT_THROW(muxer.open((output.directory / "does_not_exist" / "test.mp4").string()), EncodingError);
    EXPECT_THROW(muxer.open(output.path()), EncodingError);
    Muxer good;
    good.addStream(encoder.codecParameters(), encoder.timeBase()); good.open(output.path());
    EXPECT_THROW(good.addStream(encoder.codecParameters(), encoder.timeBase()), EncodingError);
    EXPECT_THROW(good.setStreamDuration(stream, -1, encoder.timeBase()), std::invalid_argument);
    good.setStreamDuration(stream, 0, encoder.timeBase()); good.finish();
}
