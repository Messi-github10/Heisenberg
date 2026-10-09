#pragma once

#include <Models/IProducer.hpp>

#include <memory>

namespace heisenberg {

enum class CanvasColor {
    Black,
    White,
};

class ProfileCanvas {
public:
    explicit ProfileCanvas(Profile profile = Profile::hd1080p24());
    ~ProfileCanvas();

    ProfileCanvas(const ProfileCanvas&) = delete;
    ProfileCanvas& operator=(const ProfileCanvas&) = delete;

    void setHardwareDecode(bool enabled);
    void setProfile(Profile profile);
    const Profile& profile() const;

    ProducerFrame frame(int64_t position, CanvasColor color);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace heisenberg
