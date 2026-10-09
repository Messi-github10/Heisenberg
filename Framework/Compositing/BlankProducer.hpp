#pragma once

#include <Models/IProducer.hpp>

#include <memory>

namespace heisenberg {

class BlankProducer final : public IProducer {
public:
    explicit BlankProducer(Profile profile = Profile::hd1080p24(),
                           int64_t length = 0);
    ~BlankProducer() override;

    BlankProducer(const BlankProducer&) = delete;
    BlankProducer& operator=(const BlankProducer&) = delete;

    void setHardwareDecode(bool enabled);
    void ensureLength(int64_t length);

    const Profile& profile() const override;
    const std::string& resource() const override;
    int64_t in() const override;
    int64_t out() const override;
    int64_t length() const override;
    int64_t position() const override;
    bool seekable() const override;
    bool seek(int64_t position) override;
    ProducerFrame getFrame(int64_t position) override;
    bool isBlank() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace heisenberg
