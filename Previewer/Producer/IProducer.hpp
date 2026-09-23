#pragma once

#include <Common/AudioFrame.hpp>
#include <Profile.hpp>

#include <cstdint>
#include <memory>
#include <string>

struct AVFrame;

namespace heisenberg {

struct ProducerFrame {
    int64_t position = 0;
    std::shared_ptr<AVFrame> video;
    std::shared_ptr<AudioFrame> audio;
    bool eof = false;

    bool hasVideo() const { return static_cast<bool>(video); }
    bool hasAudio() const { return static_cast<bool>(audio); }
};

class IProducer {
public:
    virtual ~IProducer() = default;

    virtual const Profile& profile() const = 0;
    virtual const std::string& resource() const = 0;
    virtual int64_t in() const = 0;
    virtual int64_t out() const = 0;
    virtual int64_t length() const = 0;
    virtual int64_t position() const = 0;
    virtual bool seekable() const = 0;
    virtual bool seek(int64_t position) = 0;
    virtual ProducerFrame getFrame(int64_t position) = 0;
};

} // namespace heisenberg
