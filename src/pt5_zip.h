// Plain stored (uncompressed) ZIP writer shared by the game's debug export and
// the launcher's crash report. No archiver process or dependency needed.
#pragma once
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace pt5_zip {

struct Entry { std::string name, data; };

inline uint32_t crc32(const std::string& data) {
    uint32_t crc = ~0u;
    for (unsigned char c : data) {
        crc ^= c;
        for (int bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

inline bool write(FILE* f, const std::vector<Entry>& entries) {
    auto u16 = [f](uint16_t v) { unsigned char b[] = {(unsigned char)v, (unsigned char)(v >> 8)}; fwrite(b, 1, 2, f); };
    auto u32 = [f](uint32_t v) { unsigned char b[] = {(unsigned char)v, (unsigned char)(v >> 8), (unsigned char)(v >> 16), (unsigned char)(v >> 24)}; fwrite(b, 1, 4, f); };
    struct Index { uint32_t crc, offset; };
    std::vector<Index> index;
    SYSTEMTIME st; GetLocalTime(&st);
    uint16_t time = (st.wHour << 11) | (st.wMinute << 5) | (st.wSecond / 2);
    uint16_t date = ((st.wYear - 1980) << 9) | (st.wMonth << 5) | st.wDay;
    for (const auto& e : entries) {
        Index i{crc32(e.data), (uint32_t)ftell(f)}; index.push_back(i);
        u32(0x04034B50); u16(20); u16(0); u16(0); u16(time); u16(date); u32(i.crc);
        u32((uint32_t)e.data.size()); u32((uint32_t)e.data.size()); u16((uint16_t)e.name.size()); u16(0);
        fwrite(e.name.data(), 1, e.name.size(), f); fwrite(e.data.data(), 1, e.data.size(), f);
    }
    uint32_t start = (uint32_t)ftell(f);
    for (size_t n = 0; n < entries.size(); n++) {
        const auto& e = entries[n]; const auto& i = index[n];
        u32(0x02014B50); u16(20); u16(20); u16(0); u16(0); u16(time); u16(date); u32(i.crc);
        u32((uint32_t)e.data.size()); u32((uint32_t)e.data.size()); u16((uint16_t)e.name.size());
        u16(0); u16(0); u16(0); u16(0); u32(0); u32(i.offset); fwrite(e.name.data(), 1, e.name.size(), f);
    }
    uint32_t end = (uint32_t)ftell(f);
    u32(0x06054B50); u16(0); u16(0); u16((uint16_t)entries.size()); u16((uint16_t)entries.size());
    u32(end - start); u32(start); u16(0);
    return !ferror(f) && fflush(f) == 0;
}

}  // namespace pt5_zip
