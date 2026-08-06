#include <Pipeline/DemuxSource.hpp>

#include <Common/Packet.hpp>
#include <Common/Stream.hpp>
#include <Demuxer/DemuxerFactory.hpp>
#include <Demuxer/IDemuxer.hpp>

namespace heisenberg::pipeline {

DemuxSource::DemuxSource() = default;
DemuxSource::~DemuxSource() = default;

int DemuxSource::open(const std::string& url) {
    close();
    demuxer_ = demuxer::createDemuxer();
    if (!demuxer_) return -1;

    const int result = demuxer_->open(url);
    if (result < 0) demuxer_.reset();
    eofReturned_ = false;
    return result;
}

void DemuxSource::close() {
    if (demuxer_) demuxer_->close();
    demuxer_.reset();
    eofReturned_ = false;
}

MediaFrame DemuxSource::read(uint64_t generation) {
    if (!demuxer_ || eofReturned_) return {};

    auto packet = demuxer_->readPacket();
    if (!packet) {
        eofReturned_ = true;
        return MediaFrame::eof(generation);
    }

    MediaFrame frame = MediaFrame::packet(packet, generation);
    frame.metadata.pts = packet->pts;
    frame.metadata.duration = packet->duration;
    frame.metadata.timeBaseNum = packet->timeBaseNum;
    frame.metadata.timeBaseDen = packet->timeBaseDen;
    frame.metadata.streamIndex = packet->streamIndex;
    return frame;
}

int DemuxSource::seek(double seconds, int streamIndex, int flags) {
    if (!demuxer_) return -1;
    const int result = demuxer_->seek(seconds, streamIndex, flags);
    if (result >= 0) eofReturned_ = false;
    return result;
}

const std::vector<Stream>& DemuxSource::streams() const {
    static const std::vector<Stream> empty;
    return demuxer_ ? demuxer_->streams() : empty;
}

double DemuxSource::duration() const {
    return demuxer_ ? demuxer_->duration() : -1.0;
}

bool DemuxSource::seekable() const {
    return demuxer_ && demuxer_->seekable();
}

bool DemuxSource::isOpen() const {
    return demuxer_ && demuxer_->isOpen();
}

} // namespace heisenberg::pipeline
