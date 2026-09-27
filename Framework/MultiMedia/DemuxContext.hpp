#pragma once

#include <Common/MediaFrame.hpp>
#include <Common/Stream.hpp>

#include <memory>
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
    int seek(double seconds, int streamIndex = -1, int flags = 1);

    const std::vector<Stream>& streams() const;
    double duration() const;
    bool seekable() const;
    bool isOpen() const;

private:
    std::unique_ptr<IDemuxer> demuxer_;
    bool eofReturned_ = false;
};

} // namespace demuxer
} // namespace heisenberg
