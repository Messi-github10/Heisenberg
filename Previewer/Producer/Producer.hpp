#pragma once

#include "IProducer.hpp"

#include <memory>
#include <string>

namespace heisenberg {

class Producer final : public IProducer {
public:
    explicit Producer(Profile profile = Profile::hd1080p24());
    ~Producer() override;

    Producer(const Producer&) = delete;
    Producer& operator=(const Producer&) = delete;

    bool open(const std::string& path, std::string* error = nullptr);
    void close();
    bool isOpen() const;

    void setHardwareDecode(bool enabled);

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
