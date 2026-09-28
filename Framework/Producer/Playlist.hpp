#pragma once

#include "IProducer.hpp"

#include <memory>
#include <string>
#include <vector>

namespace heisenberg {

struct PlaylistClip {
    std::string id;
    std::string resource;
    int64_t in = 0;
    int64_t out = 0;
    int64_t start = 0;

    int64_t duration() const { return out >= in ? out - in + 1 : 0; }
};

struct PlaylistFilter {
    std::string id;
    std::string graph;
    int64_t in = 0;
    int64_t out = 0;

    bool covers(int64_t position) const {
        if (in == 0 && out == 0) return true;
        return position >= in && (out == 0 || position <= out);
    }
};

class Playlist final : public IProducer {
public:
    Playlist();
    ~Playlist() override;

    Playlist(const Playlist&) = delete;
    Playlist& operator=(const Playlist&) = delete;

    void setHardwareDecode(bool enabled);
    bool loadFromJsonFile(const std::string& path, std::string* error = nullptr);
    bool loadFromJson(const std::string& text,
                      std::string* error = nullptr,
                      const std::string& baseDir = {});
    std::string toJson() const;

    const std::vector<PlaylistClip>& clips() const;
    const std::vector<PlaylistFilter>& filters() const;
    bool isBlankAt(int64_t position) const;
    const PlaylistFilter* filterAt(int64_t position) const;

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
