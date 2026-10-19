#pragma once

#include <Common/MediaFrame.hpp>
#include <Common/Stream.hpp>
#include <memory>
#include <functional>
#include <MultiMedia/Demuxer/IDemuxer.hpp>
#include <string>
#include <vector>

namespace heisenberg {
namespace demuxer {

class IDemuxer;

class DemuxContext {
public:
    DemuxContext();
    ~DemuxContext();

    int open(const std::string& url);
    void close();

    MediaFrame read(uint64_t generation);
    struct ReadResult {
        ReadStatus status = ReadStatus::Failure;
        MediaFrame frame;
        int error = 0;
    };
    ReadResult readResult(uint64_t generation);
    void setInterrupt(std::function<bool()> cancelled);
    int seek(double seconds, int streamIndex = -1, int flags = 1);

    const std::vector<Stream>& streams() const;
    double duration() const;
    bool seekable() const;
    bool isOpen() const;

private:
    std::unique_ptr<IDemuxer> demuxer_;
    bool eofReturned_ = false;
    std::function<bool()> cancelled_;
};

} // namespace demuxer
} // namespace heisenberg
