#include "LutLibrary.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <random>
#include <stdexcept>
#include <system_error>

namespace heisenberg::grade {
namespace {
namespace fs = std::filesystem;

void fail(const std::string& message) { throw std::runtime_error(message); }

bool validId(std::string_view id) {
    return id.size() == 64 && std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

// SHA-256 (FIPS 180-4), over exact file bytes. No platform crypto dependency.
std::string sha256(std::string_view bytes) {
    constexpr uint32_t k[] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    std::array<uint32_t, 8> state{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                                  0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    const auto block = [&](const unsigned char* data) {
        uint32_t w[64]{};
        for (int i = 0; i < 16; ++i)
            for (int j = 0; j < 4; ++j) w[i] = (w[i] << 8) | data[i * 4 + j];
        for (int i = 16; i < 64; ++i) {
            const uint32_t a = std::rotr(w[i-15],7) ^ std::rotr(w[i-15],18) ^ (w[i-15] >> 3);
            const uint32_t b = std::rotr(w[i-2],17) ^ std::rotr(w[i-2],19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + a + w[i-7] + b;
        }
        auto [a,b,c,d,e,f,g,h] = state;
        for (int i = 0; i < 64; ++i) {
            const uint32_t t1 = h + (std::rotr(e,6) ^ std::rotr(e,11) ^ std::rotr(e,25))
                + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const uint32_t t2 = (std::rotr(a,2) ^ std::rotr(a,13) ^ std::rotr(a,22))
                + ((a & b) ^ (a & c) ^ (b & c));
            h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        state[0]+=a; state[1]+=b; state[2]+=c; state[3]+=d;
        state[4]+=e; state[5]+=f; state[6]+=g; state[7]+=h;
    };
    std::size_t offset = 0;
    while (bytes.size() - offset >= 64) {
        block(reinterpret_cast<const unsigned char*>(bytes.data() + offset));
        offset += 64;
    }
    std::array<unsigned char,128> end{};
    const auto remaining = bytes.size() - offset;
    for (std::size_t i = 0; i < remaining; ++i) end[i] = static_cast<unsigned char>(bytes[offset+i]);
    end[remaining] = 0x80;
    const std::size_t padded = remaining < 56 ? 64 : 128;
    const uint64_t bits = static_cast<uint64_t>(bytes.size()) * 8;
    for (std::size_t i = 0; i < 8; ++i) end[padded-1-i] = static_cast<unsigned char>(bits >> (i*8));
    block(end.data());
    if (padded == 128) block(end.data()+64);
    std::string result;
    result.reserve(64);
    constexpr char hex[] = "0123456789abcdef";
    for (const auto word : state)
        for (int shift = 28; shift >= 0; shift -= 4) result.push_back(hex[(word >> shift) & 15]);
    return result;
}

bool space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }
std::string_view trim(std::string_view text) {
    while (!text.empty() && space(text.front())) text.remove_prefix(1);
    while (!text.empty() && space(text.back())) text.remove_suffix(1);
    return text;
}
std::string_view token(std::string_view& text) {
    text = trim(text);
    auto end = std::find_if(text.begin(), text.end(), space);
    auto result = text.substr(0, static_cast<std::size_t>(end-text.begin()));
    text.remove_prefix(result.size());
    text = trim(text);
    return result;
}
float number(std::string_view value) {
    if (!value.empty() && value.front() == '+') {
        value.remove_prefix(1);
        if (value.empty() || value.front() == '+' || value.front() == '-') fail("LUT contains an invalid float sign");
    }
    float result = 0;
    const auto parsed = std::from_chars(value.data(), value.data()+value.size(), result, std::chars_format::general);
    if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data()+value.size() || !std::isfinite(result))
        fail("LUT contains an invalid or non-finite float");
    return result;
}
std::array<float,3> triple(std::string_view text) {
    std::array<float,3> result{};
    for (auto& value : result) value = number(token(text));
    if (!text.empty()) fail("LUT requires exactly three values per row");
    return result;
}

void parse(LutAsset& asset) {
    if (asset.cubeText.empty()) fail("LUT file is empty");
    if (asset.cubeText.find('\0') != std::string::npos) fail("LUT contains a NUL byte");
    std::string_view input(asset.cubeText);
    if (input.starts_with("\xef\xbb\xbf")) input.remove_prefix(3);
    bool sizeSeen = false, minSeen = false, maxSeen = false, titleSeen = false, dataSeen = false;
    std::size_t expected = 0;
    while (!input.empty()) {
        const auto newline = input.find('\n');
        auto line = input.substr(0, newline);
        input.remove_prefix(newline == input.npos ? input.size() : newline+1);
        if (line.size() > LutLibrary::MaxLineBytes) fail("LUT line exceeds the limit");
        line = trim(line);
        if (line.empty() || line.front() == '#') continue;
        auto rest = line;
        const auto key = token(rest);
        if (key == "TITLE") {
            if (dataSeen || titleSeen) fail("Duplicate or misplaced LUT TITLE");
            titleSeen = true;
            if (rest.empty() || rest.front() != '"') fail("LUT TITLE must be quoted");
            rest.remove_prefix(1);
            const auto close = rest.find('"');
            if (close == rest.npos) fail("Unterminated LUT TITLE");
            asset.metadata.title = std::string(rest.substr(0,close));
            rest = trim(rest.substr(close+1));
            if (!rest.empty() && rest.front() != '#') fail("Unexpected text after LUT TITLE");
            continue;
        }
        const auto comment = line.find('#');
        line = trim(line.substr(0,comment));
        rest = line;
        const auto directive = token(rest);
        if (directive == "LUT_3D_SIZE") {
            if (sizeSeen || dataSeen) fail("Duplicate or misplaced LUT_3D_SIZE");
            sizeSeen = true;
            uint32_t size = 0;
            const auto parsed = std::from_chars(rest.data(), rest.data()+rest.size(),size);
            if (parsed.ec != std::errc{} || parsed.ptr != rest.data()+rest.size() || size < LutLibrary::MinSize || size > LutLibrary::MaxSize)
                fail("LUT_3D_SIZE must be an integer from 2 to 128");
            asset.metadata.size = size;
            expected = static_cast<std::size_t>(size) * size * size;
            asset.samples.reserve(expected);
        } else if (directive == "DOMAIN_MIN" || directive == "DOMAIN_MAX") {
            bool& seen = directive == "DOMAIN_MIN" ? minSeen : maxSeen;
            if (seen || dataSeen) fail("Duplicate or misplaced LUT DOMAIN directive");
            seen = true;
            (directive == "DOMAIN_MIN" ? asset.metadata.domainMin : asset.metadata.domainMax) = triple(rest);
        } else {
            if (!directive.empty() && ((directive.front() >= 'A' && directive.front() <= 'Z') || directive.front() == '_'))
                fail("Unsupported LUT directive: only standalone 3D cubes are supported");
            if (!sizeSeen) fail("LUT sample data precedes LUT_3D_SIZE");
            if (asset.samples.size() >= expected) fail("Too many LUT samples");
            asset.samples.push_back(triple(line));
            dataSeen = true;
        }
    }
    if (!sizeSeen || asset.samples.size() != expected) fail("LUT sample count does not match LUT_3D_SIZE cubed");
    for (std::size_t i = 0; i < 3; ++i)
        if (!(asset.metadata.domainMax[i] > asset.metadata.domainMin[i])) fail("Each LUT DOMAIN_MAX must exceed DOMAIN_MIN");
}

std::string readBounded(const fs::path& path) {
    std::error_code ec;
    if (!fs::is_regular_file(path,ec) || ec) fail("LUT resource is missing or is not a regular file");
    const auto size = fs::file_size(path,ec);
    if (ec || size > LutLibrary::MaxFileBytes) fail("LUT file exceeds the byte limit or cannot be read");
    std::ifstream file(path,std::ios::binary);
    if (!file) fail("Cannot open LUT file");
    std::string result;
    result.reserve(static_cast<std::size_t>(size));
    std::array<char,64*1024> chunk{};
    while (file) {
        file.read(chunk.data(),static_cast<std::streamsize>(chunk.size()));
        const auto count = static_cast<std::size_t>(file.gcount());
        if (count > LutLibrary::MaxFileBytes-result.size()) fail("LUT file exceeds the byte limit");
        result.append(chunk.data(),count);
    }
    if (!file.eof()) fail("Failed reading LUT file");
    return result;
}

void requireManagedFile(const fs::path& path) {
    std::error_code ec;
    const auto status = fs::symlink_status(path,ec);
    if (ec || !fs::is_regular_file(status)) fail("Managed LUT is missing, a symlink or not a regular file");
}
std::shared_ptr<LutAsset> load(const fs::path& path, std::string_view id) {
    requireManagedFile(path);
    auto asset = std::make_shared<LutAsset>();
    asset->cubeText = readBounded(path);
    asset->hash = sha256(asset->cubeText);
    if (asset->hash != id) fail("Managed LUT content hash does not match its identity");
    asset->id = asset->hash;
    asset->resourcePath = path;
    parse(*asset);
    asset->displayName = asset->metadata.title.empty() ? asset->id : asset->metadata.title;
    return asset;
}

// A private, exclusively-created directory avoids temporary-file races. A hard
// link publishes the complete closed file atomically and cannot replace an
// existing target on Windows or POSIX. Filesystems without hard links fail
// explicitly instead of falling back to a non-atomic/overwriting copy.
void publish(const fs::path& root, const fs::path& destination, std::string_view bytes) {
    fs::create_directories(root);
    static std::atomic<uint64_t> sequence{0};
    std::random_device random;
    fs::path temporary;
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        auto candidate = root / (".lut-import-" + std::to_string(random()) + "-" + std::to_string(sequence++));
        std::error_code ec;
        if (fs::create_directory(candidate,ec)) { temporary = std::move(candidate); break; }
        if (ec && ec != std::errc::file_exists) fail("Cannot create LUT staging directory: " + ec.message());
    }
    if (temporary.empty()) fail("Cannot allocate LUT staging directory");
    struct Cleanup {
        fs::path directory;
        ~Cleanup() {
            std::error_code ignored;
            fs::remove(directory / "asset.cube",ignored);
            fs::remove(directory,ignored);
        }
    } cleanup{temporary};
    const auto staged = temporary / "asset.cube";
    std::ofstream file(staged,std::ios::binary | std::ios::trunc);
    if (!file) fail("Cannot create staged LUT file");
    file.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));
    file.close();
    if (!file) fail("Cannot write staged LUT file");
    std::error_code ec;
    fs::create_hard_link(staged,destination,ec);
    if (ec && ec != std::errc::file_exists) fail("Cannot atomically publish LUT: " + ec.message());
}

struct Service {
    std::mutex mutex;
    std::shared_ptr<LutLibrary> library;
};
Service& service() { static Service value; return value; }
std::shared_ptr<LutLibrary> configuredLibrary() {
    auto& value = service();
    std::lock_guard lock(value.mutex);
    return value.library;
}
void report(std::string* error, const std::exception& exception) { if (error) *error = exception.what(); }
} // namespace

struct LutLibrary::Impl {
    explicit Impl(fs::path value) : root(std::move(value)) {}
    const fs::path root;
    mutable std::mutex mutex;
    mutable std::map<std::string, Snapshot, std::less<>> cache;
    mutable std::size_t cacheBytes = 0;
    static std::size_t snapshotBytes(const Snapshot& asset) {
        return sizeof(LutAsset) + 256 // Shared ownership, map key/node and allocator overhead.
            + asset->cubeText.capacity() + asset->samples.capacity() * sizeof(std::array<float,3>)
            + asset->id.capacity() + asset->hash.capacity() + asset->displayName.capacity()
            + asset->metadata.title.capacity() + asset->resourcePath.native().capacity()*sizeof(fs::path::value_type);
    }
    void remember(const Snapshot& asset) const {
        const auto cost = snapshotBytes(asset);
        // Whole-cache eviction keeps the accounting bounded and preserves all
        // snapshots already held by clients, without a second reference layer.
        if (cacheBytes + cost > MaxCacheBytes) { cache.clear(); cacheBytes = 0; }
        const auto [it,inserted] = cache.emplace(asset->id,asset);
        if (inserted) cacheBytes += cost;
    }
};

std::string_view lutDomainName(LutDomain domain) noexcept {
    switch (domain) { case LutDomain::Rec709: return "rec709"; case LutDomain::ACEScct: return "acescct"; }
    return {};
}
bool parseLutDomain(std::string_view name, LutDomain& domain) noexcept {
    if (name == "rec709") { domain = LutDomain::Rec709; return true; }
    if (name == "acescct") { domain = LutDomain::ACEScct; return true; }
    return false;
}

LutLibrary::LutLibrary(fs::path root)
    : impl_(std::make_shared<Impl>(root.empty() ? fs::path{} : fs::absolute(root).lexically_normal())) {}
const fs::path& LutLibrary::root() const noexcept { return impl_->root; }

LutLibrary::Snapshot LutLibrary::importFile(const fs::path& source, std::string* error) {
    if (error) error->clear();
    try {
        if (impl_->root.empty()) fail("LUT library root is not configured");
        auto extension = source.extension().string();
        std::transform(extension.begin(),extension.end(),extension.begin(), [](unsigned char c) {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c-'A'+'a') : static_cast<char>(c);
        });
        if (extension != ".cube") fail("Only .cube LUT files are supported");
        auto asset = std::make_shared<LutAsset>();
        asset->cubeText = readBounded(source);
        parse(*asset);
        asset->id = asset->hash = sha256(asset->cubeText);
        asset->displayName = asset->metadata.title.empty() ? source.stem().string() : asset->metadata.title;
        asset->resourcePath = impl_->root / (asset->id + ".cube");
        std::lock_guard lock(impl_->mutex);
        publish(impl_->root,asset->resourcePath,asset->cubeText);
        // Even deduplication verifies disk bytes; an existing corrupt file is
        // never silently repaired or overwritten, nor hidden by a cache hit.
        requireManagedFile(asset->resourcePath);
        if (sha256(readBounded(asset->resourcePath)) != asset->id) fail("Existing managed LUT content hash does not match its identity");
        const auto found = impl_->cache.find(asset->id);
        if (found != impl_->cache.end()) return found->second;
        impl_->remember(asset);
        return asset;
    } catch (const std::exception& exception) { report(error,exception); return {}; }
}

LutLibrary::Snapshot LutLibrary::resolve(std::string_view id, std::string* error) const {
    if (error) error->clear();
    try {
        if (impl_->root.empty()) fail("LUT library root is not configured");
        if (!validId(id)) fail("LUT identity must be 64 lowercase hexadecimal SHA-256 digits");
        std::lock_guard lock(impl_->mutex);
        const auto found = impl_->cache.find(id);
        if (found != impl_->cache.end()) return found->second;
        auto asset = load(impl_->root / (std::string(id)+".cube"),id);
        impl_->remember(asset);
        return asset;
    } catch (const std::exception& exception) { report(error,exception); return {}; }
}
void LutLibrary::invalidate(std::string_view id) const {
    std::lock_guard lock(impl_->mutex);
    if (id.empty()) { impl_->cache.clear(); impl_->cacheBytes = 0; return; }
    const auto found = impl_->cache.find(id);
    if (found == impl_->cache.end()) return;
    impl_->cacheBytes -= Impl::snapshotBytes(found->second);
    impl_->cache.erase(found);
}
void LutLibrary::setDefaultRoot(fs::path root) {
    auto library = root.empty() ? nullptr : std::make_shared<LutLibrary>(std::move(root));
    auto& value = service();
    std::lock_guard lock(value.mutex);
    value.library = std::move(library);
}
fs::path LutLibrary::configuredRoot() {
    const auto library = configuredLibrary();
    return library ? library->root() : fs::path{};
}
LutLibrary::Snapshot LutLibrary::resolveConfigured(std::string_view id, std::string* error) {
    const auto library = configuredLibrary();
    if (library) return library->resolve(id,error);
    if (error) *error = "LUT library root is not configured";
    return {};
}
void LutLibrary::invalidateConfigured(std::string_view id) {
    const auto library = configuredLibrary();
    if (library) library->invalidate(id);
}

} // namespace heisenberg::grade
