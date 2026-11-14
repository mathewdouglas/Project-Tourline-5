// Virtual install. GT5's first-boot install copies GT.VOL entries into
// USRDIR/PDIPFS byte for byte (6.24 GiB of a 6.5 GB 01.04 install). Such a
// copy is replaced by a 256-byte record naming the disc volume (as a guest
// path) and the range; the engine's cellFs serves the file from the disc
// (runtime libs/filesystem/volmap.h, PS3_FS_VOLMAP). scripts/volmap.py is the
// reference implementation of the same format; scripts/test/volmap_fs.py tests
// both. No keys and no table of contents: files are matched by bytes, and
// every match is verified in full before a record replaces a file.
// PT5_VOLMAP=0 keeps full copies.
#pragma once
#include <cstdint>
#include <cstring>
#include <string_view>
#include <unordered_map>

namespace volmap {
constexpr uint64_t record_size = 256, key_size = 64, align = 0x800, min_size = 4096;
constexpr uint64_t no_offset = UINT64_MAX;
constexpr char magic[8] = {'P', 'T', '5', 'V', 'M', 'A', 'P', '1'};
constexpr const char* guest_volume = "/dev_bdvd/PS3_GAME/USRDIR/GT.VOL";

inline uint32_t crc32(uint32_t crc, const unsigned char* p, size_t n) {
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

inline bool is_record(const fs::path& p) {
    std::error_code ec;
    if (fs::file_size(p, ec) != record_size || ec) return false;
    char b[8] = {};
    std::ifstream f(p, std::ios::binary);
    return f.read(b, 8) && std::memcmp(b, magic, 8) == 0;
}

// For each file of at least min_size under root: a 0x800-aligned position of
// the volume whose first 64 bytes equal the file's. Verified later.
struct Plan {
    fs::path volume;
    uint64_t volume_size = 0, bytes = 0;
    std::unordered_map<std::wstring, uint64_t> offset;   // by absolute file path
    uint64_t at(const fs::path& p) const {
        const auto it = offset.find(p.lexically_normal().wstring());
        return it == offset.end() ? no_offset : it->second;
    }
};

inline Plan plan(const fs::path& root, const fs::path& volume, const std::atomic<bool>& cancel,
                 const std::function<void(double)>& progress) {
    Plan result; result.volume = volume;
    result.volume_size = fs::file_size(volume);
    struct Candidate { std::wstring path; uint64_t size; std::string key; };
    std::unordered_map<size_t, std::vector<Candidate>> by_key;
    size_t pending = 0;
    if (!fs::is_directory(root)) return result;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (cancel) return result;
        if (!entry.is_regular_file()) continue;
        const auto size = entry.file_size();
        if (size < std::max(min_size, key_size) || size > result.volume_size) continue;
        std::string key(key_size, '\0');
        std::ifstream f(entry.path(), std::ios::binary);
        if (!f.read(key.data(), key_size)) continue;
        if (std::memcmp(key.data(), magic, 8) == 0 && size == record_size) continue;
        by_key[std::hash<std::string_view>{}(key)].push_back({entry.path().lexically_normal().wstring(), size, key});
        ++pending;
    }
    if (!pending) return result;
    std::ifstream vol(volume, std::ios::binary);
    if (!vol) fail(L"Cannot read the disc volume: " + volume.wstring());
    constexpr uint64_t chunk = 64ull << 20;
    std::vector<char> buffer(chunk + key_size);
    for (uint64_t pos = 0; pos < result.volume_size && pending; pos += chunk) {
        if (cancel) return result;
        vol.clear(); vol.seekg(static_cast<std::streamoff>(pos));
        vol.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const uint64_t got = static_cast<uint64_t>(vol.gcount());
        if (got < key_size) break;
        const uint64_t limit = std::min<uint64_t>(chunk, got - key_size + 1);
        for (uint64_t i = 0; i < limit; i += align) {
            const std::string_view key(buffer.data() + i, key_size);
            const auto bucket = by_key.find(std::hash<std::string_view>{}(key));
            if (bucket == by_key.end()) continue;
            auto& list = bucket->second;
            for (auto it = list.begin(); it != list.end();) {
                if (it->key == key && pos + i + it->size <= result.volume_size) {
                    result.offset[it->path] = pos + i;
                    result.bytes += it->size;
                    it = list.erase(it); --pending;
                } else ++it;
            }
            if (list.empty()) by_key.erase(bucket);
        }
        if (progress) progress(std::min(1.0, double(pos + chunk) / double(result.volume_size)));
    }
    return result;
}

// Compares file with its planned range; on a full match returns the record.
inline bool make_record(const fs::path& file, uint64_t offset, const Plan& plan, std::string& record,
                        const std::atomic<bool>& cancel) {
    std::error_code ec;
    const uint64_t size = fs::file_size(file, ec);
    if (ec || offset == no_offset || offset + size > plan.volume_size) return false;
    std::ifstream a(file, std::ios::binary), b(plan.volume, std::ios::binary);
    if (!a || !b) return false;
    b.seekg(static_cast<std::streamoff>(offset));
    std::vector<char> x(8u << 20), y(8u << 20);
    unsigned char head[16] = {}, tail[16] = {};
    const size_t edge = static_cast<size_t>(std::min<uint64_t>(16, size));
    uint32_t crc = 0;
    for (uint64_t done = 0; done < size;) {
        if (cancel) return false;
        const auto n = static_cast<size_t>(std::min<uint64_t>(x.size(), size - done));
        if (!a.read(x.data(), n) || !b.read(y.data(), n) || std::memcmp(x.data(), y.data(), n) != 0) return false;
        if (done == 0) std::memcpy(head, x.data(), std::min(edge, n));
        crc = crc32(crc, reinterpret_cast<const unsigned char*>(x.data()), n);
        done += n;
        if (done == size) {
            // The last 16 bytes may straddle two reads; read them again.
            std::ifstream t(file, std::ios::binary);
            t.seekg(static_cast<std::streamoff>(size - edge));
            if (!t.read(reinterpret_cast<char*>(tail), static_cast<std::streamsize>(edge))) return false;
        }
    }
    record.assign(record_size, '\0');
    auto put = [&](size_t at, uint64_t v, int bytes) { for (int i = 0; i < bytes; ++i) record[at + i] = char(v >> (8 * i)); };
    std::memcpy(record.data(), magic, 8);
    put(8, 1, 4); put(12, record_size, 4); put(16, offset, 8); put(24, size, 8); put(32, crc, 8);
    std::memcpy(record.data() + 40, head, 16); std::memcpy(record.data() + 56, tail, 16);
    put(72, plan.volume_size, 8);
    std::memcpy(record.data() + 80, guest_volume, std::strlen(guest_volume));
    return true;
}

// Writes data to path through "<path>.vmtmp" and a rename, so path is always
// either the old file or the new one.
inline void replace_file(const fs::path& path, const std::string& data) {
    const fs::path tmp = path.wstring() + L".vmtmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(data.data(), static_cast<std::streamsize>(data.size()));
        f.flush();
        if (!f) fail(L"Could not write " + tmp.wstring());
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::error_code ec; fs::remove(tmp, ec);
        fail(L"Could not replace " + path.wstring());
    }
}

struct Result { uint64_t files = 0, bytes = 0; bool cancelled = false; };

// Replaces verified copies under root with records, in place.
inline Result convert(const fs::path& root, const fs::path& volume, const std::atomic<bool>& cancel,
                      const std::function<void(double)>& progress) {
    Result r;
    const auto p = plan(root, volume, cancel, [&](double f) { if (progress) progress(0.5 * f); });
    uint64_t checked = 0;
    for (const auto& [path, offset] : p.offset) {
        if (cancel) { r.cancelled = true; return r; }
        std::string record;
        const fs::path file(path);
        const auto size = fs::file_size(file);
        if (make_record(file, offset, p, record, cancel)) {
            replace_file(file, record);
            ++r.files; r.bytes += size;
        }
        checked += size;
        if (progress) progress(0.5 + 0.5 * double(checked) / double(std::max<uint64_t>(p.bytes, 1)));
    }
    r.cancelled = cancel;
    return r;
}

// Count and bytes of the ordinary (non-record) files under root: unchanged
// since the last conversion means there is nothing new to convert.
inline std::wstring signature(const fs::path& root) {
    uint64_t files = 0, bytes = 0;
    if (!fs::is_directory(root)) return L"none";
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file() || is_record(entry.path())) continue;
        ++files; bytes += entry.file_size();
    }
    return std::to_wstring(files) + L":" + std::to_wstring(bytes);
}

// Only with an engine that serves records (pt5-release.ini volmap=1): an older
// engine would read the 256-byte records as the files themselves.
inline bool enabled() {
    return env(L"PT5_VOLMAP") != L"0" && profile(install/L"pt5-release.ini", L"game", L"volmap") == L"1";
}
}  // namespace volmap
