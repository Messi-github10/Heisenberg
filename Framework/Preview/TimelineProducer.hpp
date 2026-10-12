#pragma once

#include <Models/IProducer.hpp>
#include <Models/MediaManifest.hpp>
#include <Models/Timeline.hpp>

#include <memory>
#include <string>
#include <vector>

namespace heisenberg {

class TimelineProducer final : public IProducer {
public:
    TimelineProducer(Timeline timeline, MediaManifest manifest, std::string projectDir = {});
    ~TimelineProducer() override;
    TimelineProducer(TimelineProducer&&) noexcept;
    TimelineProducer& operator=(TimelineProducer&&) noexcept;
    TimelineProducer(const TimelineProducer&) = delete;
    TimelineProducer& operator=(const TimelineProducer&) = delete;

    bool open(std::string* error = nullptr);
    void setHardwareDecode(bool enabled);
    const std::vector<std::string>& warnings() const;

    const Profile& profile() const override;
    const std::string& resource() const override;
    int64_t in() const override;
    int64_t out() const override;
    int64_t length() const override;
    int64_t position() const override;
    bool seekable() const override;
    bool seek(int64_t position) override;
    ProducerFrame getFrame(int64_t position) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace heisenberg
