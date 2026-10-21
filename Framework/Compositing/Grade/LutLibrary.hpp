#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace heisenberg::grade {

// Creative processing domain, explicitly selected by the user, never inferred
// from a cube TITLE, DOMAIN_MIN/MAX, filename or sample values. Rec709 means
// encoded Rec.709 primaries with Rec.1886 transfer, not scene-linear Rec.709.
enum class LutDomain { Rec709, ACEScct };
std::string_view lutDomainName(LutDomain domain) noexcept;
bool parseLutDomain(std::string_view name, LutDomain& domain) noexcept;

struct LutMetadata {
    uint32_t size = 0;
    // DOMAIN_* applies to inputs only. Samples are never normalized or clamped.
    std::array<float, 3> domainMin{0.0f, 0.0f, 0.0f};
    std::array<float, 3> domainMax{1.0f, 1.0f, 1.0f};
    std::string title;
};

struct LutAsset {
    std::string id;   // Lowercase SHA-256 of the exact, validated source bytes.
    std::string hash; // Same value as id; no pathname participates in identity.
    std::string displayName;
    std::filesystem::path resourcePath;
    LutMetadata metadata;
    // Retained immutable data makes a held snapshot stable across invalidation,
    // source deletion and external file edits. Red changes fastest in .cube.
    std::string cubeText;
    std::vector<std::array<float, 3>> samples;
};

class LutLibrary {
public:
    using Snapshot = std::shared_ptr<const LutAsset>;
    static constexpr uint32_t MinSize = 2;
    static constexpr uint32_t MaxSize = 128;
    static constexpr std::size_t MaxFileBytes = 128u * 1024u * 1024u;
    static constexpr std::size_t MaxLineBytes = 16u * 1024u;
    static constexpr std::size_t MaxCacheBytes = 256u * 1024u * 1024u;

    // Caller supplies an app-owned root. An empty root is unconfigured; there
    // is deliberately no cwd, environment, Qt or project-directory fallback.
    explicit LutLibrary(std::filesystem::path root);
    const std::filesystem::path& root() const noexcept;

    // Only standalone 3D .cube files are accepted. Validate before publishing
    // root/<id>.cube atomically, without replacing any existing file. TITLE is
    // the display name when present, otherwise the import filename's stem.
    Snapshot importFile(const std::filesystem::path& source, std::string* error = nullptr);

    // First resolution reads, parses and verifies identity. Subsequent calls
    // share a bounded, strong-reference cache without filesystem I/O. The root
    // is trusted immutable app storage: an external edit/removal requires
    // invalidate(), after which missing/corrupt content fails closed. Cache
    // eviction may also cause revalidation. No API deletes stored assets.
    Snapshot resolve(std::string_view id, std::string* error = nullptr) const;
    void invalidate(std::string_view id = {}) const;

    // Thread-safe service lookup for preview/export. The application injects
    // its app-data root here; changing it drops the service's old cache but
    // never invalidates snapshots held by an active render/export.
    static void setDefaultRoot(std::filesystem::path root);
    static std::filesystem::path configuredRoot();
    static Snapshot resolveConfigured(std::string_view id, std::string* error = nullptr);
    static void invalidateConfigured(std::string_view id = {});

    // Projects persist only id, name, explicit domain and intensity. Neither
    // resourcePath, cubeText nor samples belongs in a project or project copy.
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace heisenberg::grade
