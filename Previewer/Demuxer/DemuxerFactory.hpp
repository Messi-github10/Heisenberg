//
// Created by NiceFold on 2026/6/21.
//

#pragma once

#include <Demuxer/IDemuxer.hpp>

#include <memory>

namespace heisenberg {
namespace demuxer {

std::unique_ptr<IDemuxer> createDemuxer();

} // namespace demuxer
} // namespace heisenberg
