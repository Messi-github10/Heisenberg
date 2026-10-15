#pragma once

#include <Common/MediaFrame.hpp>
#include <MultiMedia/Video/Decoder/DecoderFactory.hpp>

#include <memory>
#include <deque>

struct AVFrame;

namespace heisenberg {

class Stream;

namespace decoder {

class IDecoder;

class DecoderContext {
public:
    DecoderContext();
    ~DecoderContext();

    int open(const Stream& stream,
             const DecoderConfig& config = {});
    void close();
    void flush();

    bool push(const MediaFrame& input);
    MediaFrame pull(uint64_t generation);

    bool isOpen() const;
    // Sticky negative error until open/flush/close; never EAGAIN or EOF.
    int lastError() const { return lastError_; }

private:
    struct AudioConverter;

    std::unique_ptr<IDecoder> decoder_;
    std::unique_ptr<AudioConverter> audioConverter_;
    int streamIndex_ = -1;
    int lastError_ = 0;
    bool audio_ = false;
    bool draining_ = false;
    bool eofReturned_ = false;
    // Frames pulled to satisfy send_packet EAGAIN are returned before newer
    // decoder output. The input packet is retried, never silently discarded.
    std::deque<std::shared_ptr<AVFrame>> pendingFrames_;
};

} // namespace decoder
} // namespace heisenberg
