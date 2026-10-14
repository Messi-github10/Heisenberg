#pragma once

#include "Rational.hpp"

#include <cstdint>

namespace heisenberg {

struct VideoSpec {
    int width = 0;
    int height = 0;
    Rational frameRate;
    Rational sampleAspect{1, 1};
    Rational timeBase{1, 1};
    int format = -1;

    bool valid() const { return width > 0 && height > 0; }
};

} // namespace heisenberg
