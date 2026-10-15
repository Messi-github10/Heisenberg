#pragma once
#include <MultiMedia/Video/Encoder/Encoding.hpp>

namespace heisenberg {

class Muxer {
public:
    // Explicit MP4 default. MOV/Matroska etc may be named where supported.
    explicit Muxer(const std::string& format = "mp4");
    ~Muxer();
    Muxer(const Muxer&) = delete;
    Muxer& operator=(const Muxer&) = delete;
    // Before open only; deep copy parameters. Returns stable stream index.
    int addStream(const CodecParameters& parameters, AVRational timeBase);
    void open(const std::string& path); // UTF-8, writes header; single-use
    // Copies packet, rescaling from packet.timeBase to actual post-header
    // stream timebase. Preserves negative PTS/DTS and all packet side data.
    // Caller owns the packet and may destroy/reuse it immediately afterward.
    void write(const EncodedPacket& packet, int stream);
    AVRational streamTimeBase(int stream) const;
    // Optional exact presentation duration (e.g. AudioEncoder::samplesSubmitted
    // in {1,48000}). Call before finish; excludes priming and tail padding.
    void setStreamDuration(int stream, int64_t duration, AVRational timeBase);
    // Explicit trailer + IO flush + close, with errors reported. Idempotent on
    // success. Destructor only cleans up; it never silently writes a trailer.
    void finish();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace heisenberg
