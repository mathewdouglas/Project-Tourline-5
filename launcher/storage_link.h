// A fixed name for the selected storage folder, for tools that cannot read storage.ini.
//
// RecompLauncher edits gt5p_settings.toml through the paths in recomplauncher.json. Those are
// static, and the manifest is replaced by every update, so instead of rewriting it the launcher
// keeps <home>\selected-storage as a directory junction to relocated storage. The manifest uses
// that path when it exists and the default storage folder otherwise. Junctions need no
// administrator rights or Developer Mode. PT5_STORAGE_LINK=0 leaves the link alone.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#include <algorithm>
#include <cstddef>
#include <cwchar>
#include <filesystem>
#include <string>
#include <vector>

namespace storage_link {

inline const wchar_t* name() { return L"selected-storage"; }

namespace detail {
// The mount-point layout of REPARSE_DATA_BUFFER (ntifs.h is not available to user mode).
struct MountPoint {
    DWORD tag;
    WORD data_length;
    WORD reserved;
    WORD substitute_offset, substitute_length, print_offset, print_length;
    WCHAR path[1];
};
constexpr DWORD kHeader = 8;  // tag, data_length, reserved

inline bool is_link(const std::filesystem::path& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) && (a & FILE_ATTRIBUTE_REPARSE_POINT);
}

// The junction's target as stored, or empty when it is not a junction.
inline std::wstring target(const std::filesystem::path& link) {
    HANDLE h = CreateFileW(link.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::vector<BYTE> buf(MAXIMUM_REPARSE_DATA_BUFFER_SIZE);
    DWORD got = 0;
    const BOOL ok = DeviceIoControl(h, FSCTL_GET_REPARSE_POINT, nullptr, 0, buf.data(), (DWORD)buf.size(), &got, nullptr);
    CloseHandle(h);
    if (!ok || got < sizeof(MountPoint)) return {};
    const auto* m = reinterpret_cast<const MountPoint*>(buf.data());
    if (m->tag != IO_REPARSE_TAG_MOUNT_POINT) return {};
    if (offsetof(MountPoint, path) + m->substitute_offset + m->substitute_length > got) return {};
    std::wstring s(m->path + m->substitute_offset / sizeof(WCHAR), m->substitute_length / sizeof(WCHAR));
    if (s.rfind(L"\\??\\", 0) == 0) s.erase(0, 4);
    return s;
}

inline bool create(const std::filesystem::path& link, const std::wstring& to) {
    const std::wstring substitute = L"\\??\\" + to;
    const size_t chars = substitute.size() + 1 + to.size() + 1;
    const size_t data_length = 8 + chars * sizeof(WCHAR);  // four WORD offsets/lengths, then the names
    if (data_length > MAXIMUM_REPARSE_DATA_BUFFER_SIZE - kHeader) return false;
    std::vector<BYTE> buf(kHeader + data_length);
    auto* m = reinterpret_cast<MountPoint*>(buf.data());
    m->tag = IO_REPARSE_TAG_MOUNT_POINT;
    m->data_length = (WORD)data_length;
    m->substitute_offset = 0;
    m->substitute_length = (WORD)(substitute.size() * sizeof(WCHAR));
    m->print_offset = (WORD)((substitute.size() + 1) * sizeof(WCHAR));
    m->print_length = (WORD)(to.size() * sizeof(WCHAR));
    std::copy(substitute.begin(), substitute.end(), m->path);
    m->path[substitute.size()] = 0;
    std::copy(to.begin(), to.end(), m->path + substitute.size() + 1);
    m->path[substitute.size() + 1 + to.size()] = 0;
    if (!CreateDirectoryW(link.c_str(), nullptr)) return false;
    HANDLE h = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    bool ok = false;
    if (h != INVALID_HANDLE_VALUE) {
        DWORD unused = 0;
        ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, buf.data(), (DWORD)buf.size(), nullptr, 0, &unused, nullptr) != 0;
        CloseHandle(h);
    }
    if (!ok) RemoveDirectoryW(link.c_str());  // only the empty directory made above
    return ok;
}
}  // namespace detail

// Point <home>\selected-storage at `storage`, or remove it when `storage` is empty (storage is the
// default folder). Only a junction is ever removed; a real folder of that name is left alone.
// Returns an empty string on success, otherwise a reason for the log. Never throws.
inline std::wstring sync(const std::filesystem::path& home, const std::filesystem::path& storage) {
    try {
        const auto link = home / name();
        const DWORD attrs = GetFileAttributesW(link.c_str());
        const bool present = attrs != INVALID_FILE_ATTRIBUTES;
        if (present && (!detail::is_link(link) || detail::target(link).empty()))
            return L"Not linking selected storage: " + link.wstring() + L" exists and is not a junction.";
        std::wstring want;
        if (!storage.empty()) {
            want = std::filesystem::absolute(storage).lexically_normal().wstring();
            // "E:\" keeps its separator; "E:\Folder\" does not need one.
            while (want.size() > 3 && (want.back() == L'\\' || want.back() == L'/')) want.pop_back();
            if (want.rfind(L"\\\\", 0) == 0) return L"Not linking selected storage: network folders cannot be junction targets.";
        }
        if (present) {
            if (!want.empty() && _wcsicmp(detail::target(link).c_str(), want.c_str()) == 0) return {};
            if (!RemoveDirectoryW(link.c_str())) return L"Could not remove the old storage link " + link.wstring() + L".";
        }
        if (want.empty()) return {};
        if (!detail::create(link, want)) return L"Could not create the storage link " + link.wstring() + L".";
        return {};
    } catch (const std::exception&) {
        return L"Could not update the storage link.";
    }
}

}  // namespace storage_link
