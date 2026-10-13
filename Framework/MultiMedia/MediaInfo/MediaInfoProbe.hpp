//
// Created by NiceFold on 2026/10/13.
//

#pragma once

#include <string>
#include <vector>

namespace heisenberg::mediainfo {

enum class StreamKind {
    General,
    Video,
    Audio,
};

struct Field {
    std::string name;
    std::string value;
};

struct StreamInfo {
    StreamKind kind = StreamKind::General;
    std::vector<Field> fields;
};

struct ProbeResult {
    bool loaded = false;
    std::string error;
    std::vector<StreamInfo> streams;
};

// Reads technical metadata through MediaInfo.dll. The call is synchronous and
// creates its own library object, so it may run on a worker thread. Objects
// are not shared across threads. Values are the raw MediaInfo strings
// (duration in milliseconds, bit rates in bits per second, sizes in bytes).
ProbeResult probeFile(const std::string& path);

} // namespace heisenberg::mediainfo
