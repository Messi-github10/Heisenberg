#pragma once
#include "Encoding.hpp"
#include <cerrno>
#include <limits>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
}

namespace heisenberg::encoding_detail {
inline void check(int result, const char* operation) {
    if (result >= 0) return;
    char error[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(result, error, sizeof(error));
    throw EncodingError(std::string(operation) + ": " + error);
}
inline void require(bool condition, const char* message) {
    if (!condition) throw std::invalid_argument(message);
}
inline void timeBase(AVRational value) {
    require(value.num > 0 && value.den > 0, "timebase must be positive");
}
struct CodecDeleter { void operator()(AVCodecContext* p) const { avcodec_free_context(&p); } };
struct FrameDeleter { void operator()(AVFrame* p) const { av_frame_free(&p); } };
struct PacketDeleter { void operator()(AVPacket* p) const { av_packet_free(&p); } };
using CodecPtr = std::unique_ptr<AVCodecContext, CodecDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
inline FramePtr frame() {
    FramePtr p(av_frame_alloc());
    if (!p) throw std::bad_alloc();
    return p;
}
inline PacketPtr packet() {
    PacketPtr p(av_packet_alloc());
    if (!p) throw std::bad_alloc();
    return p;
}

// Always empty receive queue after a send, including EAGAIN retries. Codec
// contracts forbid both send and receive returning EAGAIN without progress.
template<class Consumer>
void send(AVCodecContext* context, const AVFrame* input, Consumer&& consume) {
    auto p = packet();
    auto receive = [&]() {
        int count = 0;
        for (;;) {
            const int result = avcodec_receive_packet(context, p.get());
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
                return std::pair<int, int>{result, count};
            check(result, "avcodec_receive_packet");
            consume(*p);
            av_packet_unref(p.get());
            ++count;
        }
    };
    for (;;) {
        const int result = avcodec_send_frame(context, input);
        if (result != AVERROR(EAGAIN)) {
            check(result, "avcodec_send_frame");
            break;
        }
        const auto progress = receive();
        if (progress.second == 0 || progress.first == AVERROR_EOF)
            throw EncodingError("encoder send/receive made no progress");
    }
    const auto end = receive();
    if (!input && end.first != AVERROR_EOF)
        throw EncodingError("encoder drain did not reach EOF");
}
} // namespace heisenberg::encoding_detail
