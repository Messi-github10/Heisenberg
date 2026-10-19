#include <MultiMedia/DemuxContext.hpp>
#include <Common/Packet.hpp>
#include <Common/Stream.hpp>
#include <MultiMedia/Demuxer/DemuxerFactory.hpp>
#include <MultiMedia/Demuxer/IDemuxer.hpp>

namespace heisenberg::demuxer {

DemuxContext::DemuxContext() = default;
DemuxContext::~DemuxContext() = default;

int DemuxContext::open(const std::string& url) {
    close();
    demuxer_ = demuxer::createDemuxer();
    if (!demuxer_) return -1;

    demuxer_->setInterrupt(cancelled_);
    const int result = demuxer_->open(url);
    if (result < 0) demuxer_.reset();
    eofReturned_ = false;
    return result;
}

void DemuxContext::close() {
    if (demuxer_) demuxer_->close();
    demuxer_.reset();
    eofReturned_ = false;
}

void DemuxContext::setInterrupt(std::function<bool()> cancelled) {
    cancelled_ = std::move(cancelled);
    if (demuxer_) demuxer_->setInterrupt(cancelled_);
}
MediaFrame DemuxContext::read(uint64_t generation) {
    return readResult(generation).frame;
}
DemuxContext::ReadResult DemuxContext::readResult(uint64_t generation) {
    if (!demuxer_) return {ReadStatus::Failure, {}, -1};
    if (eofReturned_) return {ReadStatus::End, {}, 0};
    auto result = demuxer_->readPacketResult();
    if (result.status == ReadStatus::End) {
        eofReturned_ = true;
        return {ReadStatus::End, MediaFrame::eof(generation), result.error};
    }
    if (result.status != ReadStatus::Packet)
        return {result.status, {}, result.error};
    auto packet = std::move(result.packet);
    MediaFrame frame = MediaFrame::packet(packet, generation);
    frame.metadata.pts = packet->pts;
    frame.metadata.duration = packet->duration;
    frame.metadata.timeBaseNum = packet->timeBaseNum;
    frame.metadata.timeBaseDen = packet->timeBaseDen;
    frame.metadata.streamIndex = packet->streamIndex;
    return {ReadStatus::Packet, std::move(frame), 0};
}

int DemuxContext::seek(double seconds, int streamIndex, int flags) {
    if (!demuxer_) return -1;
    const int result = demuxer_->seek(seconds, streamIndex, flags);
    if (result >= 0) eofReturned_ = false;
    return result;
}

const std::vector<Stream>& DemuxContext::streams() const {
    static const std::vector<Stream> empty;
    return demuxer_ ? demuxer_->streams() : empty;
}

double DemuxContext::duration() const {
    return demuxer_ ? demuxer_->duration() : -1.0;
}

bool DemuxContext::seekable() const {
    return demuxer_ && demuxer_->seekable();
}

bool DemuxContext::isOpen() const {
    return demuxer_ && demuxer_->isOpen();
}

} // namespace heisenberg::demuxer
