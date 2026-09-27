#pragma once

#include <Common/MediaFrame.hpp>
#include <MultiMedia/Video/Decoder/DecoderFactory.hpp>

#include <memory>

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

private:
    struct AudioConverter;

    std::unique_ptr<IDecoder> decoder_;
    std::unique_ptr<AudioConverter> audioConverter_;
    int streamIndex_ = -1;
    bool audio_ = false;
    bool draining_ = false;
    bool eofReturned_ = false;
};

} // namespace decoder
} // namespace heisenberg
