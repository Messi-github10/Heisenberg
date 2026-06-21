//
// Created by NiceFold on 2026/6/21.
//

#include <Demuxer/DemuxerFactory.hpp>
#include <Demuxer/Demuxer.hpp>

#include <memory>

namespace heisenberg {
namespace demuxer {

std::unique_ptr<IDemuxer> createDemuxer() {
    return std::make_unique<Demuxer>();
}

} // namespace demuxer
} // namespace heisenberg
