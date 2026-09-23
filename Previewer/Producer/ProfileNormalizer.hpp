#pragma once

#include <Profile.hpp>

#include <memory>
#include <string>

struct AVFrame;

namespace heisenberg {

class ProfileNormalizer {
public:
    ProfileNormalizer();
    ~ProfileNormalizer();

    ProfileNormalizer(const ProfileNormalizer&) = delete;
    ProfileNormalizer& operator=(const ProfileNormalizer&) = delete;

    bool initialize(const Profile& profile, std::string* error = nullptr);
    std::shared_ptr<AVFrame> normalize(const AVFrame* source,
                                       std::string* error = nullptr);
    void shutdown();
    bool isReady() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace heisenberg
