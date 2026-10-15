#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
#include <libavutil/rational.h>
}

struct AVPacket;
struct AVCodecParameters;
struct AVCodecContext;

namespace heisenberg {

// Encoding APIs throw invalid_argument for invalid input and EncodingError for
// FFmpeg/IO/state failures. No destructor throws or implicitly finishes output.
class EncodingError : public std::runtime_error {
public:
    explicit EncodingError(const std::string& message) : std::runtime_error(message) {}
};

// Owns a complete AVPacket including side data. Timestamps remain in timeBase().
class EncodedPacket {
public:
    EncodedPacket(const AVPacket& packet, AVRational timeBase);
    ~EncodedPacket();
    EncodedPacket(EncodedPacket&&) noexcept;
    EncodedPacket& operator=(EncodedPacket&&) noexcept;
    EncodedPacket(const EncodedPacket&) = delete;
    EncodedPacket& operator=(const EncodedPacket&) = delete;
    const AVPacket* native() const noexcept;
    AVRational timeBase() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
using EncodedPackets = std::vector<EncodedPacket>;

// Deep copies extradata, channel layout and all coded side data; never maps via
// Common/Codec.hpp. A snapshot is independent of the encoder's lifetime.
class CodecParameters {
public:
    explicit CodecParameters(const AVCodecContext& context);
    explicit CodecParameters(const AVCodecParameters& parameters);
    ~CodecParameters();
    CodecParameters(const CodecParameters&);
    CodecParameters& operator=(const CodecParameters&);
    CodecParameters(CodecParameters&&) noexcept;
    CodecParameters& operator=(CodecParameters&&) noexcept;
    const AVCodecParameters* native() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace heisenberg
