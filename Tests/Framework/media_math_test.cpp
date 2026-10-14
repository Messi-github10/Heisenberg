#include <Common/MediaMath.hpp>
#include <gtest/gtest.h>

#include <cmath>
#include <limits>

using namespace heisenberg;

TEST(MediaRationalRegression, ZeroNonFiniteAndOutOfRangeAreChecked) {
    bool ok = false;
    EXPECT_EQ(Rational::fromDouble(0.0, &ok), Rational(0, 1));
    EXPECT_TRUE(ok);
    EXPECT_EQ(Rational::fromDouble(30000.0 / 1001.0, &ok), Rational(30000, 1001));
    EXPECT_TRUE(ok);
    for (const double invalid : {std::numeric_limits<double>::infinity(),
                                -std::numeric_limits<double>::infinity(),
                                std::numeric_limits<double>::quiet_NaN(), 1e100}) {
        EXPECT_TRUE(Rational::fromDouble(invalid, &ok).isNaN());
        EXPECT_FALSE(ok);
    }
    EXPECT_TRUE(Rational::NaN.isNaN());
}

TEST(MediaMathRegression, FractionalFrameBoundariesRemainExactAtLargePositions) {
    constexpr int64_t cycles = 1000000000;
    const Rational ntsc(30000, 1001);
    EXPECT_EQ(samplesToPosition(ntsc, 48000, 30000 * cycles), 48000LL * 1001 * cycles);
    EXPECT_EQ(samplesToPosition(ntsc, 48000, -30000 * cycles), -48000LL * 1001 * cycles);
    EXPECT_EQ(samplesToPosition(Rational(24000, 1001), 48000, 24000 * cycles),
              48000LL * 1001 * cycles);
    const int64_t largePosition = 30000 * cycles + 2;
    EXPECT_EQ(samplesToPosition(ntsc, 48000, largePosition), 48000LL * 1001 * cycles + 3203);
    EXPECT_EQ(samplesForFrame(ntsc, 48000, largePosition), 1602);
}

TEST(MediaMathRegression, AdjacentSampleRangesTelescopeIncludingNegativePreroll) {
    const Rational fps(30000, 1001);
    int64_t samples = 0;
    for (int64_t index = -30000; index < 30000; ++index) {
        const int count = samplesForFrame(fps, 44100, index);
        EXPECT_GE(count, 1471);
        EXPECT_LE(count, 1472);
        samples += count;
    }
    EXPECT_EQ(samples, 2LL * 44100 * 1001);
    EXPECT_EQ(samplesToPosition(Rational(2, 1), 1, 1), 1);
    EXPECT_EQ(samplesToPosition(Rational(2, 1), 1, -1), -1);
    EXPECT_EQ(samplesToPosition(30000.0 / 1001.0, 48000, 30000), 48000LL * 1001);
}

TEST(MediaMathRegression, InvalidScalesAndOverflowNeverProduceWrappedIndices) {
    EXPECT_EQ(samplesToPosition(Rational(0, 1), 48000, 42), 0);
    EXPECT_EQ(samplesToPosition(Rational(24, 1), 0, 42), 0);
    EXPECT_THROW(checkedRescale(42, AVRational{0, 1}, AVRational{1, 48000}),
                 std::invalid_argument);
    EXPECT_THROW(samplesToPosition(Rational(1, 1), 48000,
                                   std::numeric_limits<int64_t>::max()), std::overflow_error);
    EXPECT_THROW(samplesForFrame(Rational(24, 1), 48000,
                                 std::numeric_limits<int64_t>::max()), std::overflow_error);
    EXPECT_THROW(samplesForFrame(Rational(1, 1001), 4800000, 0), std::overflow_error);
}

TEST(MediaMathRegression, TimestampConversionsRejectMissingInvalidAndHugeValues) {
    AVFrame frame{};
    frame.pts = AV_NOPTS_VALUE;
    frame.time_base = {1, 90000};
    EXPECT_FALSE(hasFrameTimestamp(frame));
    EXPECT_TRUE(std::isnan(frameTimeSeconds(frame)));
    EXPECT_EQ(frameIndexFromTimestamp(frame, {30, 1}), AV_NOPTS_VALUE);
    frame.pts = 90000;
    EXPECT_EQ(frameIndexFromTimestamp(frame, {30, 1}), 30);
    EXPECT_EQ(timestampFromSeconds(1.0, {1, 90000}), 90000);
    EXPECT_EQ(timestampFromSeconds(std::numeric_limits<double>::infinity(), {1, 90000}),
              AV_NOPTS_VALUE);
    EXPECT_EQ(timestampFromSeconds(1e100, {1, 90000}), AV_NOPTS_VALUE);
    EXPECT_EQ(timestampFromSeconds(1.0, {0, 1}), AV_NOPTS_VALUE);
}
