//
// Created by NiceFold on 2026/6/21.
//

#pragma once

#include <Demuxer/IDemuxer.hpp>
#include <Common/Codec.hpp>
#include <Common/Packet.hpp>
#include <Common/Stream.hpp>
#include <memory>
#include <string>
#include <vector>

struct AVFormatContext;
struct AVStream;
struct AVPacket;
enum AVCodecID;

namespace heisenberg {
namespace demuxer {

class Demuxer final : public IDemuxer {
public:
    Demuxer();
    ~Demuxer() override;

    Demuxer(const Demuxer &) = delete;
    Demuxer &operator=(const Demuxer &) = delete;
    Demuxer(Demuxer &&) = delete;
    Demuxer &operator=(Demuxer &&) = delete;

    int open(const std::string &url) override;
    void close() override;
    std::shared_ptr<Packet> readPacket() override;
    int seek(double seconds, int streamIndex = -1, int flags = 1 /* AVSEEK_FLAG_BACKWARD */) override;

    const std::vector<Stream> &streams() const override { return streams_; }
    double duration() const override;
    bool seekable() const override;
    bool isOpen() const override { return formatCtx_ != nullptr; }
    const std::string &url() const override { return url_; }
    double startTime() const override;
    std::string formatName() const override;

private:
    static CodecParams::ID mapCodecId(enum AVCodecID id);

    static CodecParams avstreamToCodecparams(const AVStream *avstream);

    int buildStreams();

    std::shared_ptr<Packet> avpacketToPacket(const AVPacket *avpkt);

    const Stream *findStream(int streamIndex) const;

    AVFormatContext *formatCtx_ = nullptr;
    std::string url_;
    std::vector<Stream> streams_;
};

} // namespace demuxer
} // namespace heisenberg
