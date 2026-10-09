#pragma once

#include <cstdint>
#include <string>

namespace heisenberg {

struct Clip {
    std::string id;
    std::string mediaRef;
    std::string resource;
    int64_t start = 0;
    int64_t in = 0;
    int64_t out = 0;

    int64_t duration() const { return out >= in ? out - in + 1 : 0; }
    int64_t end() const { return start + duration(); }
    bool covers(int64_t position) const {
        return position >= start && position < end();
    }
};

struct TimelineFilter {
    std::string id;
    std::string graph;
    int64_t in = 0;
    int64_t out = 0;

    bool covers(int64_t position) const {
        if (in == 0 && out == 0) return true;
        return position >= in && (out == 0 || position <= out);
    }
};

} // namespace heisenberg
