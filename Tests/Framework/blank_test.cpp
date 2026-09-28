#include <Producer/BlankProducer.hpp>

#include <gtest/gtest.h>

#include <cstdint>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace {

bool isWhiteCanvas(const heisenberg::ProducerFrame& frame) {
    if (!frame.hasVideo() ||
        frame.video->width != 1920 ||
        frame.video->height != 1080 ||
        frame.video->format != AV_PIX_FMT_RGBAF16) {
        return false;
    }
    const uint16_t white = 0x3C00;
    const uint16_t* pixel = reinterpret_cast<const uint16_t*>(frame.video->data[0]);
    return pixel[0] == white && pixel[1] == white &&
           pixel[2] == white && pixel[3] == white;
}

bool isSilent(const heisenberg::ProducerFrame& frame) {
    if (!frame.hasAudio()) return false;
    if (frame.audio->spec().sampleRate != 48000) return false;
    if (frame.audio->spec().channels != 2) return false;
    if (frame.audio->samples() != 2000) return false;
    for (int channel = 0; channel < frame.audio->spec().channels; ++channel) {
        const float* samples = frame.audio->channelRo(channel);
        for (int index = 0; index < frame.audio->samples(); ++index) {
            if (samples[index] != 0.0f) return false;
        }
    }
    return true;
}

} // namespace

TEST(BlankProducerTest, GetFrameReturnsWhiteCanvasAndSilence) {
    heisenberg::BlankProducer blank(heisenberg::Profile::hd1080p24(), 10);
    EXPECT_TRUE(blank.isBlank());
    EXPECT_EQ(blank.resource(), heisenberg::kBlankResource);
    EXPECT_EQ(blank.length(), 10);
    EXPECT_EQ(blank.in(), 0);
    EXPECT_EQ(blank.out(), 9);
    EXPECT_TRUE(blank.seekable());

    const heisenberg::ProducerFrame first = blank.getFrame(0);
    const heisenberg::ProducerFrame last = blank.getFrame(9);
    ASSERT_FALSE(first.eof);
    ASSERT_FALSE(last.eof);
    EXPECT_EQ(first.position, 0);
    EXPECT_EQ(last.position, 9);
    EXPECT_TRUE(isWhiteCanvas(first));
    EXPECT_TRUE(isWhiteCanvas(last));
    EXPECT_TRUE(isSilent(first));
    EXPECT_TRUE(isSilent(last));

    blank.ensureLength(24);
    EXPECT_EQ(blank.length(), 24);
    const heisenberg::ProducerFrame extended = blank.getFrame(23);
    ASSERT_FALSE(extended.eof);
    EXPECT_TRUE(isWhiteCanvas(extended));
    EXPECT_TRUE(isSilent(extended));

    const heisenberg::ProducerFrame pastEnd = blank.getFrame(24);
    EXPECT_TRUE(pastEnd.eof);
}

TEST(BlankProducerTest, HardwareDecodeUsesD3D11Canvas) {
    heisenberg::BlankProducer blank(heisenberg::Profile::hd1080p24(), 4);
    blank.setHardwareDecode(true);

    const heisenberg::ProducerFrame frame = blank.getFrame(0);
    ASSERT_FALSE(frame.eof);
    ASSERT_TRUE(frame.hasVideo());
    EXPECT_EQ(frame.video->width, 1920);
    EXPECT_EQ(frame.video->height, 1080);
    EXPECT_TRUE(frame.video->format == AV_PIX_FMT_D3D11
                || frame.video->format == AV_PIX_FMT_RGBAF16);
    if (frame.video->format == AV_PIX_FMT_RGBAF16) {
        EXPECT_TRUE(isWhiteCanvas(frame));
    }
    EXPECT_TRUE(isSilent(frame));
}
