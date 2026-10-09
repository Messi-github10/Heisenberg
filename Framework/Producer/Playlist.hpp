#pragma once

#include "IProducer.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace heisenberg {

struct PlaylistClip {
    std::string id;
    std::string mediaRef;
    std::string resource;
    int64_t start = 0;
    int64_t in = 0;
    int64_t out = 0;

    int64_t duration() const { return out >= in ? out - in + 1 : 0; }
    int64_t end() const { return start + duration(); }
    bool covers(int64_t position) const {
        return position >= start && position < end();
    }
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
    Playlist(Playlist&&) noexcept;
    Playlist& operator=(Playlist&&) noexcept;

    void setHardwareDecode(bool enabled);
    void setProfile(Profile profile);
    bool loadFromJsonFile(const std::string& path, std::string* error = nullptr);
    bool loadFromJson(const std::string& text,
                      std::string* error = nullptr,
                      const std::string& baseDir = {},
                      const std::unordered_map<std::string, std::string>& mediaPaths = {});
    std::string toJson() const;

    bool addClip(PlaylistClip& clip, std::string* error = nullptr);
    bool removeClip(const std::string& id, std::string* error = nullptr);
    bool moveClip(const std::string& id, int64_t start, std::string* error = nullptr);

    const std::vector<PlaylistClip>& clips() const;
    const std::vector<PlaylistFilter>& filters() const;
    const PlaylistClip* clipById(const std::string& id) const;
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
