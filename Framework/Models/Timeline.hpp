#pragma once

#include "Clip.hpp"
#include "IProducer.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace heisenberg {

class Timeline final : public IProducer {
public:
    Timeline();
    ~Timeline() override;

    Timeline(const Timeline&) = delete;
    Timeline& operator=(const Timeline&) = delete;
    Timeline(Timeline&&) noexcept;
    Timeline& operator=(Timeline&&) noexcept;

    void setHardwareDecode(bool enabled);
    void setProfile(Profile profile);
    bool loadFromJsonFile(const std::string& path, std::string* error = nullptr);
    bool loadFromJson(const std::string& text,
                      std::string* error = nullptr,
                      const std::string& baseDir = {},
                      const std::unordered_map<std::string, std::string>& mediaPaths = {});
    std::string toJson() const;

    bool addClip(Clip& clip, std::string* error = nullptr);
    bool removeClip(const std::string& id, std::string* error = nullptr);
    bool moveClip(const std::string& id, int64_t start, std::string* error = nullptr);

    const std::vector<Clip>& clips() const;
    const std::vector<TimelineFilter>& filters() const;
    const Clip* clipById(const std::string& id) const;
    bool isBlankAt(int64_t position) const;
    const TimelineFilter* filterAt(int64_t position) const;

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
