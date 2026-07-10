//
// Created by NiceFold on 2026/7/11.
//

#pragma once

namespace heisenberg {

struct NonCopy {
    NonCopy()  = default;
    ~NonCopy() = default;

    NonCopy(const NonCopy&)            = delete;
    NonCopy& operator=(const NonCopy&) = delete;

    NonCopy(NonCopy&&)                 = default;
    NonCopy& operator=(NonCopy&&)      = default;
};

} // namespace heisenberg
