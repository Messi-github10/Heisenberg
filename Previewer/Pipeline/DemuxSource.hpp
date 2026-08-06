#pragma once

#include <Common/MediaFrame.hpp>
#include <Common/Stream.hpp>

#include <memory>
#include <string>
#include <vector>

namespace heisenberg {

namespace demuxer { class IDemuxer; }

namespace pipeline {

class DemuxSource {
public:
    DemuxSource();
    ~DemuxSource();

    int open(const std::string& url);
    void close();

    MediaFrame read(uint64_t generation);
    int seek(double seconds, int streamIndex = -1, int flags = 1);

    const std::vector<Stream>& streams() const;
    double duration() const;
    bool seekable() const;
    bool isOpen() const;

private:
    std::unique_ptr<demuxer::IDemuxer> demuxer_;
    bool eofReturned_ = false;
};

} // namespace pipeline
} // namespace heisenberg
