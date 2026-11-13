// Crash report: after the game exits, decide whether it crashed, froze or
// stopped with an error, and offer the player a report: a ZIP of its logs,
// settings and PC details (user and PC names removed, no game files), plus a
// pre-filled GitHub issue they submit themselves. Nothing is sent by this code.
//
// PT5_CRASH_REPORT=0: no report; the old "exited with code N" message.
// PT5_CRASH_FREEZE_MS: shortest stall counted as a freeze (default 10000).
// PT5_CRASH_REPORT_URL: the new-issue page (default PT5_ISSUES_URL).
#pragma once
#include <windows.h>
#include <shlobj.h>
#include <dxgi.h>
#include <intrin.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>
#include "../src/pt5_zip.h"
#pragma comment(lib, "advapi32.lib")   // GetUserNameW

#ifndef PT5_VERSION
#define PT5_VERSION "dev"
#endif
#ifndef PT5_REVISION
#define PT5_REVISION "unknown"
#endif
#ifndef PT5_ISSUES_URL
#define PT5_ISSUES_URL "https://github.com/mathewdouglas/Project-Tourline-5/issues/new"
#endif

namespace crash_report {
namespace fs = std::filesystem;

enum class Kind { none, crash, freeze, error };

struct Analysis {
    Kind kind = Kind::none;
    unsigned long code = 0;
    std::string what;                 // one line: "crashed (access violation, 0xC0000005)"
    std::vector<std::string> detail;  // the log lines that explain it
    std::string milestone;            // last [milestone] line: where in the game
    unsigned long long longest_stall_ms = 0;
};

inline std::string read_file(const fs::path& p, size_t limit = (size_t)-1) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (s.size() > limit) s.resize(limit);
    return s;
}

// The last `limit` bytes: crash and stall lines are at the end of a long log.
inline std::string read_tail(const fs::path& p, size_t limit) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return {};
    const std::streamoff size = f.tellg();
    const std::streamoff from = size > (std::streamoff)limit ? size - (std::streamoff)limit : 0;
    f.seekg(from);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Bounded log: the first `head` bytes (boot, versions) and the last `tail`.
inline std::string bounded(const std::string& s, size_t head = 256 * 1024, size_t tail = 3 * 1024 * 1024) {
    if (s.size() <= head + tail) return s;
    return s.substr(0, head) + "\n[... " + std::to_string(s.size() - head - tail) +
           " bytes left out of this report ...]\n" + s.substr(s.size() - tail);
}

inline std::vector<std::string> lines(const std::string& s) {
    std::vector<std::string> out; size_t at = 0;
    while (at < s.size()) {
        size_t nl = s.find('\n', at); if (nl == std::string::npos) nl = s.size();
        std::string l = s.substr(at, nl - at);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        out.push_back(std::move(l)); at = nl + 1;
    }
    return out;
}

inline const char* exception_name(unsigned long code) {
    switch (code) {
    case 0xC0000005: return "access violation";
    case 0xC00000FD: return "stack overflow";
    case 0xC0000409: return "fail-fast / stack buffer overrun";
    case 0xC0000374: return "heap corruption";
    case 0xC000001D: return "illegal instruction";
    case 0xC0000094: return "integer divide by zero";
    case 0xC0000096: return "privileged instruction";
    case 0xC0000602: return "fail-fast exception";
    case 0xC0000017: return "out of memory";
    case 0xE06D7363: return "unhandled C++ exception";
    case 0x80000003: return "breakpoint";
    default: return nullptr;
    }
}

inline bool starts(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

inline unsigned long long freeze_threshold() {
    char b[32]; DWORD n = GetEnvironmentVariableA("PT5_CRASH_FREEZE_MS", b, sizeof b);
    return n && n < sizeof b ? strtoull(b, nullptr, 10) : 10000ull;
}

// Classify a finished run from its exit code and stderr.
inline Analysis analyze(unsigned long code, const std::string& err) {
    Analysis a; a.code = code;
    const auto ls = lines(err);
    int crash_at = -1, stall_at = -1;
    for (int i = 0; i < (int)ls.size(); i++) {
        const auto& l = ls[i];
        if (starts(l, "[milestone]")) a.milestone = l;
        if (crash_at < 0 && (starts(l, "[GT5P] ACCESS VIOLATION") || starts(l, "[GT5P] EXCEPTION 0x"))) crash_at = i;
        if (starts(l, "[stall] no present for ")) {
            const unsigned long long ms = strtoull(l.c_str() + 23, nullptr, 10);
            if (ms > a.longest_stall_ms) a.longest_stall_ms = ms;
            if (stall_at < 0) stall_at = i;
        }
    }
    const bool exception_code = (code & 0xF0000000u) == 0xC0000000u || code == 0xE06D7363 || code == 0x80000003;
    char hex[16]; snprintf(hex, sizeof hex, "0x%08lX", code);
    if (exception_code || crash_at >= 0) {
        a.kind = Kind::crash;
        const char* name = exception_name(code);
        a.what = std::string("crashed (") + (name ? std::string(name) + ", " : std::string()) + hex + ")";
        if (crash_at >= 0)
            for (int i = crash_at; i < (int)ls.size() && a.detail.size() < 24; i++)
                if (starts(ls[i], "[GT5P]")) a.detail.push_back(ls[i]);
    } else if (a.longest_stall_ms >= freeze_threshold()) {
        a.kind = Kind::freeze;
        a.what = "froze (no new picture for " + std::to_string(a.longest_stall_ms / 1000) + " s)";
        if (code) a.what += ", then exited with code " + std::to_string(code);
        for (int i = stall_at; i < (int)ls.size() && a.detail.size() < 24; i++)
            if (starts(ls[i], "[stall]")) a.detail.push_back(ls[i]);
    } else if (code) {
        a.kind = Kind::error;
        a.what = "exited with code " + std::to_string(code) + (code == 3 ? " (abort)" : "") + " (" + hex + ")";
        std::vector<std::string> tail;
        for (int i = (int)ls.size() - 1; i >= 0 && tail.size() < 20; i--)
            if (!ls[i].empty()) tail.push_back(ls[i]);
        a.detail.assign(tail.rbegin(), tail.rend());
    }
    for (auto& d : a.detail) if (d.size() > 400) d = d.substr(0, 400) + " ...";
    return a;
}

// ---- Privacy: user, profile and PC names out of everything in the report ----
inline std::string narrow(const std::wstring& w, UINT cp) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(cp, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(cp, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
inline std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

// setup.ini and other profile files are UTF-16 (WritePrivateProfileStringW):
// convert them, or the byte-level scrubber below would not match names in them.
inline std::string as_utf8(const std::string& s) {
    if (s.size() < 2 || (unsigned char)s[0] != 0xFF || (unsigned char)s[1] != 0xFE) return s;
    std::wstring w((s.size() - 2) / 2, L'\0');
    memcpy(w.data(), s.data() + 2, w.size() * 2);
    return narrow(w, CP_UTF8);
}

struct Scrubber {
    struct Rule { std::string from, to; bool word; };
    std::vector<Rule> rules;
    void add(const std::wstring& w, const char* to, bool word) {
        if (w.size() < 2) return;
        for (UINT cp : {CP_UTF8, CP_ACP}) {
            std::string s = narrow(w, cp);
            if (s.empty()) continue;
            rules.push_back({s, to, word});
            if (!word) {   // the same path with the other slash
                std::string t = s; std::replace(t.begin(), t.end(), '\\', '/');
                if (t != s) rules.push_back({t, to, false});
            }
        }
    }
    static bool ieq(char a, char b) {
        return (a >= 'A' && a <= 'Z' ? a + 32 : a) == (b >= 'A' && b <= 'Z' ? b + 32 : b);
    }
    static bool wordch(char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || (unsigned char)c >= 0x80; }
    std::string apply(std::string s) const {
        for (const auto& r : rules) {
            std::string out; out.reserve(s.size());
            size_t i = 0;
            while (i < s.size()) {
                bool hit = i + r.from.size() <= s.size() &&
                    std::equal(r.from.begin(), r.from.end(), s.begin() + i, ieq);
                if (hit && r.word)
                    hit = (i == 0 || !wordch(s[i - 1])) &&
                          (i + r.from.size() == s.size() || !wordch(s[i + r.from.size()]));
                if (hit) { out += r.to; i += r.from.size(); }
                else out += s[i++];
            }
            s.swap(out);
        }
        return s;
    }
};

inline Scrubber scrubber() {
    Scrubber sc;
    PWSTR prof = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &prof)) && prof) {
        sc.add(prof, "%USERPROFILE%", false);
        CoTaskMemFree(prof);
    }
    wchar_t name[256]; DWORD n = 256;
    if (GetUserNameW(name, &n)) sc.add(name, "<user>", true);
    n = 256;
    if (GetComputerNameW(name, &n)) sc.add(name, "<pc>", true);
    return sc;
}

// ---- PC details ----
inline std::string system_info() {
    std::string s;
    char b[512];
    using RtlGetVersionFn = LONG (WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW os{}; os.dwOSVersionInfoSize = sizeof os;
    if (auto fn = (RtlGetVersionFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")) fn(&os);
    // Windows 11 still reports 10.0; its builds start at 22000.
    const bool win11 = os.dwMajorVersion == 10 && os.dwBuildNumber >= 22000;
    snprintf(b, sizeof b, "Windows %s (%lu.%lu build %lu)\n", win11 ? "11" : os.dwMajorVersion == 10 ? "10" : "",
             os.dwMajorVersion, os.dwMinorVersion, os.dwBuildNumber); s += b;
    int r[4]; char brand[49] = {};
    __cpuid(r, 0x80000000);
    if ((unsigned)r[0] >= 0x80000004u)
        for (int i = 0; i < 3; i++) { __cpuid(r, 0x80000002 + i); memcpy(brand + 16 * i, r, 16); }
    std::string cpu = brand; cpu.erase(0, cpu.find_first_not_of(' '));
    SYSTEM_INFO si; GetNativeSystemInfo(&si);
    snprintf(b, sizeof b, "CPU: %s (%lu logical processors)\n", cpu.c_str(), si.dwNumberOfProcessors); s += b;
    MEMORYSTATUSEX mem{}; mem.dwLength = sizeof mem; GlobalMemoryStatusEx(&mem);
    snprintf(b, sizeof b, "Memory: %.1f GB, %.1f GB free when the report was made\n",
             mem.ullTotalPhys / 1073741824.0, mem.ullAvailPhys / 1073741824.0); s += b;
    using CreateFactoryFn = HRESULT (WINAPI*)(REFIID, void**);
    if (HMODULE dx = LoadLibraryW(L"dxgi.dll")) {
        IDXGIFactory1* f = nullptr;
        auto create = (CreateFactoryFn)GetProcAddress(dx, "CreateDXGIFactory1");
        if (create && SUCCEEDED(create(__uuidof(IDXGIFactory1), (void**)&f)) && f) {
            IDXGIAdapter1* a = nullptr;
            for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
                DXGI_ADAPTER_DESC1 d{}; a->GetDesc1(&d);
                LARGE_INTEGER umd{};
                const bool ver = SUCCEEDED(a->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd));
                if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                    snprintf(b, sizeof b, "GPU: %s, %.1f GB, driver %u.%u.%u.%u\n", narrow(d.Description, CP_UTF8).c_str(),
                             d.DedicatedVideoMemory / 1073741824.0,
                             ver ? HIWORD(umd.HighPart) : 0, ver ? LOWORD(umd.HighPart) : 0,
                             ver ? HIWORD(umd.LowPart) : 0, ver ? LOWORD(umd.LowPart) : 0);
                    s += b;
                }
                a->Release();
            }
            f->Release();
        }
        FreeLibrary(dx);
    }
    return s;
}

// ---- The issue text ----
inline const char* kind_title(Kind k) {
    return k == Kind::crash ? "Crash" : k == Kind::freeze ? "Freeze" : "Error exit";
}

inline std::string summary(const Analysis& a, const std::string& sys, const std::string& zip_name, size_t budget = 2400) {
    std::string s;
    s += "**What happened:** the game " + a.what + ".\n\n";
    s += "**What were you doing?** (please describe: mode, track, car, what you pressed)\n\n\n";
    s += "**Where:** " + (a.milestone.empty() ? std::string("unknown") : a.milestone) + "\n\n";
    s += "**Version:** Project Tourline 5 " PT5_VERSION " (" PT5_REVISION ")\n\n";
    s += "**PC:**\n```\n" + sys + "```\n\n";
    if (!a.detail.empty() && budget) {
        s += "<details><summary>Log lines</summary>\n\n```\n";
        for (const auto& d : a.detail) {
            if (d.size() + 1 > budget) { s += "...\n"; break; }
            s += d + "\n"; budget -= d.size() + 1;
        }
        s += "```\n</details>\n\n";
    }
    s += "**Report file:** please attach `" + zip_name + "` (drag it into this box).\n";
    return s;
}

inline std::string url_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}

inline std::string issue_url(const Analysis& a, const std::string& body) {
    char b[1024]; DWORD n = GetEnvironmentVariableA("PT5_CRASH_REPORT_URL", b, sizeof b);
    std::string base = n && n < sizeof b ? std::string(b, n) : std::string(PT5_ISSUES_URL);
    std::string title = std::string(kind_title(a.kind)) + ": " + a.what;
    return base + "?title=" + url_encode(title) + "&body=" + url_encode(body);
}

struct Report { fs::path zip; std::string url, body; };

// Write <data>/logs/reports/<kind>-<time>.zip. Throws std::exception on failure.
inline Report write(const fs::path& data, const Analysis& a) {
    const Scrubber sc = scrubber();
    SYSTEMTIME st; GetLocalTime(&st);
    char stamp[64];
    snprintf(stamp, sizeof stamp, "%s-%04u%02u%02u-%02u%02u%02u", a.kind == Kind::crash ? "crash" : a.kind == Kind::freeze ? "freeze" : "error",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    const fs::path dir = data / L"logs" / L"reports";
    fs::create_directories(dir);
    Report r; r.zip = dir / (std::string(stamp) + ".zip");
    const std::string sys = sc.apply(system_info());
    Analysis shown = a;
    for (auto& d : shown.detail) d = sc.apply(d);
    shown.milestone = sc.apply(shown.milestone);
    // GitHub refuses very long new-issue links; the full logs are in the ZIP.
    for (size_t budget : {2400u, 1200u, 500u, 0u}) {
        r.body = summary(shown, sys, r.zip.filename().string(), budget);
        r.url = issue_url(shown, r.body);
        if (r.url.size() <= 7000) break;
    }
    std::vector<pt5_zip::Entry> e;
    e.push_back({"README.txt",
        "Project Tourline 5 problem report. It holds the game's logs from the run that\r\n"
        "failed, your settings and basic PC details. Your Windows user name, profile\r\n"
        "folder and PC name were replaced before it was written; no game files, saves\r\n"
        "or screenshots are included. Open the files to check before sharing.\r\n"});
    e.push_back({"summary.md", r.body});
    e.push_back({"system.txt", sys + "Exit code: " + std::to_string(a.code) + "\nLauncher: " PT5_VERSION " (" PT5_REVISION ")\n"});
    for (const char* name : {"play.err", "play.out"}) {
        const fs::path p = data / L"logs" / name;
        if (fs::is_regular_file(p)) e.push_back({name, sc.apply(as_utf8(bounded(read_file(p))))});
    }
    for (const wchar_t* name : {L"gt5p_settings.toml", L"setup.ini"}) {
        const fs::path p = data / name;
        if (fs::is_regular_file(p)) e.push_back({narrow(name, CP_UTF8), sc.apply(as_utf8(read_file(p, 256 * 1024)))});
    }
    const fs::path tmp = r.zip.wstring() + L".part";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) throw std::runtime_error("could not create the report file");
    bool ok = pt5_zip::write(f, e);
    if (fclose(f)) ok = false;
    if (!ok) { std::error_code ec; fs::remove(tmp, ec); throw std::runtime_error("could not write the report file"); }
    fs::rename(tmp, r.zip);
    return r;
}

inline bool enabled() {
    wchar_t b[8]; DWORD n = GetEnvironmentVariableW(L"PT5_CRASH_REPORT", b, 8);
    return !(n && b[0] == L'0');
}

}  // namespace crash_report
