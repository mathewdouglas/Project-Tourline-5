// Native first-run launcher. No game code, decryption keys or game data belong here.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <commctrl.h>
#include <bcrypt.h>
#include <winhttp.h>
#include <uiautomation.h>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iterator>
#include <vector>
#include <string>
#include <array>
#include <cstdio>
#include <algorithm>
#include <cwctype>
#include <atomic>
#include <thread>
#include <mutex>
#include <exception>
#include "pt5_launcher_config.h"
#include "storage_link.h"
#include "../src/pt5_restart.h"
#include "crash_report.h"

namespace fs = std::filesystem;
struct Failure { std::wstring message; };
[[noreturn]] static void fail(const std::wstring& s) { throw Failure{s}; }
struct Handle {
    HANDLE h = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE v) : h(v) {}
    ~Handle() { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    Handle(const Handle&) = delete;
};
static fs::path install, data;
static bool quiet = false;
static bool silent_errors = false;
static bool easy_setup = false;
// RecompLauncher consumes these lines from its redirected preparation stdout.
static void setup_progress(double fraction, const char* message) {
    if (!easy_setup) return;
    printf("progress %.3f %s\n", fraction, message);
    fflush(stdout);
}
static std::wstring profile(const fs::path& file, const wchar_t* section, const wchar_t* key) {
    std::array<wchar_t, 32768> b{};
    const DWORD n = GetPrivateProfileStringW(section, key, L"", b.data(), (DWORD)b.size(), file.c_str());
    if (n == b.size() - 1) fail(L"A configuration path is too long.");
    return b.data();
}
static void setting(const fs::path& file, const wchar_t* key, const fs::path& value) {
    if (!WritePrivateProfileStringW(L"game", key, value.c_str(), file.c_str()))
        fail(L"Could not save setup: " + file.wstring());
}
static std::wstring env(const wchar_t* key) {
    DWORD n = GetEnvironmentVariableW(key, nullptr, 0);
    if (!n) return {};
    std::vector<wchar_t> b(n);
    GetEnvironmentVariableW(key, b.data(), n);
    return b.data();
}
static void setenv(const wchar_t* key, const std::wstring& value) {
    if (!SetEnvironmentVariableW(key, value.c_str())) fail(L"Could not set launch environment.");
}
// Stable path for RecompLauncher's settings and folder buttons. Never blocks play.
static void link_storage(const fs::path& home) {
    if (env(L"PT5_STORAGE_LINK") == L"0") return;
    const auto problem = storage_link::sync(home, data == home ? fs::path{} : data);
    if (problem.empty()) return;
    fwprintf(stderr, L"%ls\n", problem.c_str());
    try {
        fs::create_directories(data/L"logs");
        std::wofstream(data/L"logs/storage-link.err", std::ios::app) << problem << L'\n';
    } catch (...) {}
}
static HICON brand_icon() {
    static HICON icon = reinterpret_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101),
        IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0));
    return icon;
}
static void brand(TASKDIALOGCONFIG& td) {
    HICON icon = brand_icon();
    if (!icon) return;
    td.dwFlags |= TDF_USE_HICON_MAIN;
    td.hMainIcon = icon;
}
// The current runtime's VFS uses ANSI CRT file paths. Reject lossy conversion
// instead of accepting a path which will silently refer to a different file.
static void runtime_path(const fs::path& p) {
    BOOL lossy = FALSE;
    UINT cp = GetACP();
    int n = WideCharToMultiByte(cp, cp == CP_UTF8 ? WC_ERR_INVALID_CHARS : WC_NO_BEST_FIT_CHARS,
        p.c_str(), -1, nullptr, 0, nullptr, cp == CP_UTF8 ? nullptr : &lossy);
    if (!n || lossy || n >= 480)
        fail(L"The current game runtime cannot use this path. Choose a shorter folder with characters supported by your Windows language:\n\n" + p.wstring());
}
static std::wstring hash(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) fail(L"Cannot read: " + file.wstring());
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE ctx = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        fail(L"Windows SHA-256 is unavailable.");
    DWORD bytes = 0, got = 0;
    NTSTATUS rc = BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&bytes, sizeof bytes, &got, 0);
    std::vector<unsigned char> obj(bytes), block(65536);
    std::array<unsigned char, 32> digest{};
    if (rc >= 0) rc = BCryptCreateHash(alg, &ctx, obj.data(), bytes, nullptr, 0, 0);
    while (rc >= 0 && in) {
        in.read((char*)block.data(), block.size());
        if (in.gcount()) rc = BCryptHashData(ctx, block.data(), (ULONG)in.gcount(), 0);
    }
    if (rc >= 0 && !in.bad()) rc = BCryptFinishHash(ctx, digest.data(), (ULONG)digest.size(), 0);
    else rc = (NTSTATUS)0xC0000001L;
    if (ctx) BCryptDestroyHash(ctx);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (rc < 0) fail(L"Could not hash: " + file.wstring());
    std::wstring out;
    for (auto b : digest) { out += L"0123456789abcdef"[b >> 4]; out += L"0123456789abcdef"[b & 15]; }
    return out;
}
static std::vector<unsigned char> read_small(const fs::path& p) {
    if (!fs::is_regular_file(p) || fs::file_size(p) > 65536) fail(L"Missing or invalid PARAM.SFO:\n\n"+p.wstring());
    std::ifstream f(p, std::ios::binary);
    return std::vector<unsigned char>(std::istreambuf_iterator<char>(f), {});
}
static unsigned le(const std::vector<unsigned char>& b, size_t at, size_t n) {
    if (at > b.size() || n > b.size() - at) fail(L"Malformed PARAM.SFO.");
    unsigned v = 0;
    for (size_t i = 0; i < n; ++i) v |= unsigned(b[at+i]) << (8*i);
    return v;
}
static std::string text_at(const std::vector<unsigned char>& b, size_t at, size_t n) {
    if (at > b.size() || n > b.size() - at) fail(L"Malformed PARAM.SFO.");
    std::string s;
    for (size_t i = 0; i < n && b[at+i]; ++i) s += char(b[at+i]);
    return s;
}
static std::string sfo_value(const fs::path& p, const char* wanted) {
    auto b = read_small(p);
    if (le(b, 0, 4) != 0x46535000) fail(L"This is not a valid PS3 PARAM.SFO.");
    const size_t keys = le(b, 8, 4), values = le(b, 12, 4), count = le(b, 16, 4);
    if (count > 256 || 20 + count * 16 > b.size() || keys > b.size() || values > b.size()) fail(L"Malformed PARAM.SFO table.");
    std::string result;
    for (size_t i = 0; i < count; ++i) {
        const size_t at = 20 + i * 16, key = keys + le(b, at, 2);
        if (key >= b.size()) fail(L"Malformed PARAM.SFO key.");
        auto name = text_at(b, key, b.size() - key);
        const auto size = le(b, at + 4, 4), maximum = le(b, at + 8, 4);
        if (size > maximum) fail(L"Malformed PARAM.SFO value.");
        auto value = text_at(b, values + le(b, at + 12, 4), size);
        if (name == wanted) result = value;
    }
    return result;
}
static fs::path game_folder(fs::path p) {
    if (!fs::exists(p / L"PARAM.SFO") && fs::exists(p / L"PS3_GAME")) p /= L"PS3_GAME";
    if (sfo_value(p / L"PARAM.SFO", "TITLE_ID") != "BCUS98114" ||
        sfo_value(p / L"PARAM.SFO", "VERSION") != "02.00")
        fail(L"Select the extracted Gran Turismo 5 US disc (BCUS98114, disc version 02.00), or its PS3_GAME folder. Prologue and other regions are not supported by this build.");
    if (!fs::is_regular_file(p / L"USRDIR" / L"GT.VOL") || !fs::file_size(p / L"USRDIR" / L"GT.VOL"))
        fail(L"The selected dump is missing USRDIR\\GT.VOL. Extract the ISO first.");
    p = fs::canonical(p); runtime_path(p); return p;
}
static fs::path installed_folder(fs::path chosen) {
    std::vector<fs::path> candidates = {chosen, chosen / L"dev_hdd0/game/BCUS98114", chosen / L"game/BCUS98114",
        chosen / L"BCUS98114", chosen / L"gamedata/dev_hdd0/game/BCUS98114"};
    if (chosen.filename() == L"USRDIR") candidates.push_back(chosen.parent_path());
    for (const auto& p : candidates) {
        if (!fs::is_directory(p / L"USRDIR/PDIPFS") || fs::is_empty(p / L"USRDIR/PDIPFS")) continue;
        if (fs::is_regular_file(p / L"PARAM.SFO") &&
            (sfo_value(p / L"PARAM.SFO", "TITLE_ID") != "BCUS98114" || sfo_value(p / L"PARAM.SFO", "APP_VER") != "01.04"))
            fail(L"The installed game data is not GT5 BCUS98114 update 01.04. Select the matching installation, not a newer update.");
        return fs::canonical(p);
    }
    fail(L"No installed GT5 data was found. Select your RPCS3 folder or dev_hdd0\\game\\BCUS98114 containing USRDIR\\PDIPFS. Install update 01.04 in RPCS3 first. A completed GT5 install can also be imported.");
}
static fs::path pick(bool folder, const wchar_t* title, const wchar_t* label = L"Decrypted executable", const wchar_t* pattern = L"*.elf") {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        fail(L"Windows file picker could not be opened.");
    DWORD flags = FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR;
    dialog->SetOptions(flags | (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
    COMDLG_FILTERSPEC filter{label, pattern};
    if (!folder) dialog->SetFileTypes(1, &filter);
    dialog->SetTitle(title);
    HRESULT hr = dialog->Show(nullptr);
    fs::path result;
    if (SUCCEEDED(hr)) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) { result = path; CoTaskMemFree(path); }
            item->Release();
        }
    }
    dialog->Release();
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return {};
    if (FAILED(hr) || result.empty()) fail(L"The selected path could not be read.");
    return result;
}
// Windows argv quoting, not shell quoting. No user paths are sent to cmd.exe.
static std::wstring quote(const fs::path& p) {
    std::wstring out = L"\"";
    unsigned slashes = 0;
    for (wchar_t c : p.wstring()) {
        if (c == L'\\') { ++slashes; continue; }
        out.append(slashes * (c == L'"' ? 2 : 1), L'\\'); slashes = 0;
        if (c == L'"') out += L'\\';
        out += c;
    }
    out.append(slashes * 2, L'\\');
    return out + L'"';
}

#include "storage.h"

// Starts a child in a kill-on-close job with its output in logs/<tag>.*.
static void start_child(const fs::path& exe, const std::wstring& args, const fs::path& cwd, const wchar_t* tag,
                        Handle& job_out, Handle& process_out, DWORD* pid = nullptr) {
    fs::create_directories(data / L"logs");
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    Handle out(CreateFileW((data/L"logs"/(std::wstring(tag)+L".out")).c_str(), GENERIC_WRITE,
        FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    Handle err(CreateFileW((data/L"logs"/(std::wstring(tag)+L".err")).c_str(), GENERIC_WRITE,
        FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ|FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr));
    if (out.h == INVALID_HANDLE_VALUE || err.h == INVALID_HANDLE_VALUE || input.h == INVALID_HANDLE_VALUE)
        fail(L"Could not open logs in " + (data/L"logs").wstring());
    STARTUPINFOW si{}; si.cb = sizeof si; si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = input.h; si.hStdOutput = out.h; si.hStdError = err.h;
    Handle job(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job.h || !SetInformationJobObject(job.h, JobObjectExtendedLimitInformation, &limits, sizeof limits))
        fail(L"Could not create the application process group.");
    PROCESS_INFORMATION pi{}; std::wstring command = quote(exe) + L" " + args;
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW|CREATE_SUSPENDED,
                        nullptr, cwd.c_str(), &si, &pi))
        fail(L"Could not start " + exe.wstring() + L" (Windows error " + std::to_wstring(GetLastError()) + L"). Rebuild or re-extract the complete player folder.");
    Handle process(pi.hProcess), thread(pi.hThread);
    if (!AssignProcessToJobObject(job.h, process.h)) {
        TerminateProcess(process.h, 1); // Only our suspended child.
        fail(L"Could not attach the launched application to its process group.");
    }
    if (ResumeThread(thread.h) == static_cast<DWORD>(-1)) fail(L"Could not resume the launched application.");
    if (pid) *pid = pi.dwProcessId;
    std::swap(job_out.h, job.h); std::swap(process_out.h, process.h);
}
static DWORD run(const fs::path& exe, const std::wstring& args, const fs::path& cwd, const wchar_t* tag) {
    Handle job(INVALID_HANDLE_VALUE), process(INVALID_HANDLE_VALUE);
    start_child(exe, args, cwd, tag, job, process);
    for (;;) {
        const DWORD waited = MsgWaitForMultipleObjects(1, &process.h, FALSE, INFINITE, QS_ALLINPUT);
        if (waited == WAIT_OBJECT_0) break;
        if (waited == WAIT_FAILED) fail(L"Could not wait for the launched application.");
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    DWORD code = 1; GetExitCodeProcess(process.h, &code); return code;
}
// After a crash, freeze or error exit: offer a report (crash_report.h). False
// when there is nothing to report or reporting is off, so the caller keeps
// its old message. Silent setups (PT5_SETUP_UI=0) save the ZIP without asking.
static bool offer_report(DWORD code) {
    if (!crash_report::enabled()) return false;
    const auto a = crash_report::analyze(code, crash_report::read_tail(data/L"logs/play.err", 32ull << 20));
    if (a.kind == crash_report::Kind::none) return false;
    if (silent_errors) {
        try {
            const auto r = crash_report::write(data, a);
            fwprintf(stderr, L"The game %ls. Report saved: %ls\n", crash_report::widen(a.what).c_str(), r.zip.c_str());
            if (const auto url_file = env(L"PT5_CRASH_REPORT_URL_FILE"); !url_file.empty())
                std::ofstream(fs::path(url_file), std::ios::binary) << r.url;
        } catch (const std::exception&) { fwprintf(stderr, L"The game stopped; the report could not be saved.\n"); }
        return true;
    }
    const std::wstring heading = a.kind == crash_report::Kind::crash ? L"Gran Turismo 5 crashed"
                               : a.kind == crash_report::Kind::freeze ? L"Gran Turismo 5 froze"
                               : L"Gran Turismo 5 stopped with an error";
    const std::wstring body = L"The game " + crash_report::widen(a.what) + L".\n\n"
        L"A report helps get this fixed. It holds the game's logs from this run, your settings and basic PC "
        L"details (Windows version, processor, graphics card, memory). Your Windows user name, profile folder "
        L"and PC name are removed, and no game files, saves or screenshots are included.\n\n"
        L"Nothing is sent automatically: GitHub opens in your browser with a summary filled in, and you "
        L"attach the report file and submit it yourself.";
    TASKDIALOG_BUTTON buttons[] = {
        {100, L"Report this on GitHub\nSaves the report, shows it in Explorer and opens a new GitHub issue (needs a GitHub account)."},
        {101, L"Save the report only\nSaves it and shows it in Explorer, to share another way."}};
    TASKDIALOGCONFIG td{}; td.cbSize = sizeof td;
    td.dwFlags = static_cast<TASKDIALOG_FLAGS>(TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION);
    td.pszWindowTitle = L"Project Tourline 5 - problem report";
    td.pszMainInstruction = heading.c_str(); td.pszContent = body.c_str();
    td.cButtons = 2; td.pButtons = buttons; td.dwCommonButtons = TDCBF_CLOSE_BUTTON; brand(td);
    int choice = IDCLOSE;
    if (FAILED(TaskDialogIndirect(&td, &choice, nullptr, nullptr)) || (choice != 100 && choice != 101)) return true;
    crash_report::Report r;
    try { r = crash_report::write(data, a); }
    catch (const std::exception&) { fail(L"The report could not be saved. The game's logs are in:\n\n" + (data/L"logs").wstring()); }
    const std::wstring select = L"/select," + quote(r.zip);
    ShellExecuteW(nullptr, L"open", L"explorer.exe", select.c_str(), nullptr, SW_SHOWNORMAL);
    if (choice == 100)
        ShellExecuteW(nullptr, L"open", crash_report::widen(r.url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return true;
}
// ---- Setup-owned RPCS3 -----------------------------------------------------
// When the player supplies only the disc, setup downloads one pinned official
// RPCS3 build into <storage>/tools/rpcs3, uses it to install the disc's bundled
// package and decrypt its executable, then removes it. RPCS3 holds the keys;
// none are in this launcher. PT5_RPCS3_DOWNLOAD=0 turns this off,
// PT5_RPCS3_ARCHIVE=<file> uses a local copy of the pinned archive instead of
// downloading it, PT5_KEEP_RPCS3=1 keeps the tool after setup.
static bool managed_rpcs3_enabled() { return env(L"PT5_RPCS3_DOWNLOAD") != L"0"; }
static fs::path tools_root() { return data / L"tools"; }
static void remove_tools() {
    const auto root = tools_root();
    if (!fs::exists(root)) return;
    if (GetFileAttributesW(root.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT)
        fail(L"The setup tools folder is a link; remove it by hand: " + root.wstring());
    for (const auto& entry : fs::recursive_directory_iterator(root))
        if (GetFileAttributesW(entry.path().c_str()) & FILE_ATTRIBUTE_REPARSE_POINT)
            fail(L"The setup tools folder contains a link; remove it by hand: " + root.wstring());
    fs::remove_all(root);
}
struct Internet {
    HINTERNET h = nullptr;
    explicit Internet(HINTERNET v) : h(v) {}
    ~Internet() { if (h) WinHttpCloseHandle(h); }
    Internet(const Internet&) = delete;
};
// HTTPS GET into a file. Follows redirects; progress through state.
static void http_download(const std::wstring& url, const fs::path& destination, CopyState& state) {
    URL_COMPONENTS parts{}; parts.dwStructSize = sizeof parts;
    std::wstring host(256, L'\0'), path(2048, L'\0');
    parts.lpszHostName = host.data(); parts.dwHostNameLength = static_cast<DWORD>(host.size());
    parts.lpszUrlPath = path.data(); parts.dwUrlPathLength = static_cast<DWORD>(path.size());
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS)
        fail(L"Invalid download address: " + url);
    host.resize(parts.dwHostNameLength); path.resize(parts.dwUrlPathLength);
#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif
    Internet session(WinHttpOpen(L"ProjectTourline5-setup", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                 WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.h) session.h = WinHttpOpen(L"ProjectTourline5-setup", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    Internet connection(session.h ? WinHttpConnect(session.h, host.c_str(), parts.nPort, 0) : nullptr);
    Internet request(connection.h ? WinHttpOpenRequest(connection.h, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr);
    const std::wstring offline = L"Could not download " + url +
        L"\n\nCheck your internet connection, then run setup again. Windows error ";
    if (!request.h || !WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.h, nullptr))
        fail(offline + std::to_wstring(GetLastError()) + L".");
    DWORD status = 0, size = sizeof status;
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) fail(L"Could not download " + url + L" (server answered " + std::to_wstring(status) + L"). Run setup again later.");
    std::ofstream out(destination, std::ios::binary | std::ios::trunc);
    if (!out) fail(L"Could not write " + destination.wstring());
    std::vector<char> buffer(1 << 16);
    ULONGLONG last = 0;
    for (;;) {
        if (state.cancel) throw Cancelled{};
        DWORD got = 0;
        if (!WinHttpReadData(request.h, buffer.data(), static_cast<DWORD>(buffer.size()), &got))
            fail(offline + std::to_wstring(GetLastError()) + L".");
        if (!got) break;
        out.write(buffer.data(), got);
        if (!out) fail(L"Could not write " + destination.wstring() + L". Check free space.");
        state.copied += got;
        if (easy_setup && GetTickCount64() - last >= 250) {
            last = GetTickCount64();
            setup_progress(0.03 + 0.02 * state.copied / std::max<uint64_t>(state.total, 1), "Downloading RPCS3 (used only to unpack your disc's update)");
        }
    }
}
// Download (or take PT5_RPCS3_ARCHIVE / an earlier download) and check the pinned identity.
static fs::path pinned_file(const std::wstring& url, const fs::path& destination, const std::wstring& sha256,
                            uint64_t bytes, const std::wstring& local_override, const wchar_t* what) {
    const fs::path local = local_override.empty() ? destination : fs::absolute(local_override);
    if (!fs::is_regular_file(local) || fs::file_size(local) != bytes || hash(local) != sha256) {
        if (!local_override.empty())
            fail(local.wstring() + L" is not the pinned " + what + L" this launcher was built for. Remove the override or supply the matching file.");
        CopyState state; state.total = bytes;
        {
            std::lock_guard<std::mutex> guard(state.mutex);
            state.current = std::wstring(L"Downloading the official ") + what +
                L" build that setup uses to unpack the update on your disc. It is removed when setup finishes.";
        }
        with_progress(L"Preparing Gran Turismo 5", state.current.c_str(), state, [&] { http_download(url, destination, state); });
        if (fs::file_size(destination) != bytes || hash(destination) != sha256) {
            std::error_code ignored; fs::remove(destination, ignored);
            fail(std::wstring(L"The ") + what + L" download was incomplete or changed. Run setup again.");
        }
    }
    return local;
}
static fs::path managed_rpcs3() {
    const auto dir = tools_root() / L"rpcs3", exe = dir / L"rpcs3.exe", stamp = dir / L"pt5-rpcs3.txt";
    if (fs::is_regular_file(exe) && fs::is_regular_file(stamp)) {
        std::wifstream in(stamp); std::wstring pinned; std::getline(in, pinned);
        if (pinned == PT5_RPCS3_SHA256) return exe;
    }
    setup_progress(0.03, "Downloading RPCS3 (used only to unpack your disc's update)");
    if (fs::exists(dir)) remove_tools();
    fs::create_directories(dir);
    const auto archive = pinned_file(PT5_RPCS3_URL, tools_root() / L"rpcs3.7z", PT5_RPCS3_SHA256, PT5_RPCS3_BYTES,
                                     env(L"PT5_RPCS3_ARCHIVE"), L"RPCS3");
    setup_progress(0.05, "Unpacking RPCS3");
    std::array<wchar_t, MAX_PATH> system{};
    const UINT length = GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size()));
    const fs::path tar = length ? fs::path(system.data()) / L"tar.exe" : fs::path{};
    // Windows 11's tar reads 7z; older Windows uses 7-Zip's own pinned extractor.
    if (!tar.empty() && fs::is_regular_file(tar) && env(L"PT5_RPCS3_NO_TAR") != L"1")
        run(tar, L"-xf " + quote(archive) + L" -C " + quote(dir), dir, L"unpack-rpcs3");
    if (!fs::is_regular_file(exe)) {
        const auto sevenzip = pinned_file(PT5_SEVENZIP_URL, tools_root() / L"7zr.exe", PT5_SEVENZIP_SHA256,
                                          PT5_SEVENZIP_BYTES, env(L"PT5_SEVENZIP_EXE"), L"7-Zip extractor");
        run(sevenzip, L"x -y " + quote(L"-o" + dir.wstring()) + L" " + quote(archive), dir, L"unpack-rpcs3");
    }
    if (!fs::is_regular_file(exe)) fail(L"RPCS3 could not be unpacked. See logs\\unpack-rpcs3.err, then run setup again.");
    std::error_code ignored;
    if (archive == tools_root() / L"rpcs3.7z") fs::remove(archive, ignored);
    // Nothing but the install dialog setup answers: no welcome or notice boxes.
    fs::create_directories(dir / L"GuiConfigs");
    std::ofstream(dir / L"GuiConfigs/CurrentSettings.ini", std::ios::binary)
        << "[main_window]\r\ninfoBoxEnabledWelcome=false\r\ninfoBoxEnabledInstallPUP=false\r\ninfoBoxEnabledInstallPKG=false\r\n";
    std::wofstream(stamp) << PT5_RPCS3_SHA256 << L'\n';
    return exe;
}
// ---- Answering RPCS3's package dialog --------------------------------------
template <class T> struct Com {
    T* p = nullptr;
    ~Com() { if (p) p->Release(); }
    T** put() { if (p) { p->Release(); p = nullptr; } return &p; }
    T* operator->() const { return p; }
};
static Com<IUIAutomationElement> find_named(IUIAutomation* uia, IUIAutomationElement* root, CONTROLTYPEID type, const wchar_t* name) {
    Com<IUIAutomationElement> found;
    VARIANT t{}; t.vt = VT_I4; t.lVal = type;
    VARIANT n{}; n.vt = VT_BSTR; n.bstrVal = SysAllocString(name);
    Com<IUIAutomationCondition> by_type, by_name, both;
    if (SUCCEEDED(uia->CreatePropertyCondition(UIA_ControlTypePropertyId, t, by_type.put())) &&
        SUCCEEDED(uia->CreatePropertyCondition(UIA_NamePropertyId, n, by_name.put())) &&
        SUCCEEDED(uia->CreateAndCondition(by_type.p, by_name.p, both.put())))
        root->FindFirst(TreeScope_Descendants, both.p, found.put());
    VariantClear(&n);
    return found;
}
// RPCS3's "PKG Installation" dialog: untick "Precompile caches" (which would
// compile the whole game in RPCS3) and press Install. True once pressed.
static bool press_install(IUIAutomation* uia, DWORD pid) {
    Com<IUIAutomationElement> root; Com<IUIAutomationCondition> mine; Com<IUIAutomationElementArray> windows;
    VARIANT v{}; v.vt = VT_I4; v.lVal = static_cast<LONG>(pid);
    if (FAILED(uia->GetRootElement(root.put())) || FAILED(uia->CreatePropertyCondition(UIA_ProcessIdPropertyId, v, mine.put())) ||
        FAILED(root->FindAll(TreeScope_Children, mine.p, windows.put())) || !windows.p) return false;
    int count = 0; windows->get_Length(&count);
    for (int i = 0; i < count; ++i) {
        Com<IUIAutomationElement> window;
        if (FAILED(windows->GetElement(i, window.put()))) continue;
        auto install_button = find_named(uia, window.p, UIA_ButtonControlTypeId, L"Install");
        if (!install_button.p) continue;
        auto precompile = find_named(uia, window.p, UIA_CheckBoxControlTypeId, L"Precompile caches");
        Com<IUIAutomationTogglePattern> toggle;
        if (precompile.p && SUCCEEDED(precompile->GetCurrentPatternAs(UIA_TogglePatternId, IID_PPV_ARGS(toggle.put()))) && toggle.p) {
            ToggleState state = ToggleState_Off;
            if (SUCCEEDED(toggle->get_CurrentToggleState(&state)) && state == ToggleState_On) toggle->Toggle();
        }
        Com<IUIAutomationInvokePattern> invoke;
        if (SUCCEEDED(install_button->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(invoke.put()))) && invoke.p &&
            SUCCEEDED(invoke->Invoke())) return true;
    }
    return false;
}
// The package's files are all present: the pinned count and total size.
static bool package_installed(const fs::path& game) {
    if (!fs::is_regular_file(game / L"PARAM.SFO")) return false;
    uint64_t files = 0, bytes = 0; std::error_code error;
    for (fs::recursive_directory_iterator it(game, error), end; !error && it != end; it.increment(error))
        if (it->is_regular_file(error)) { ++files; bytes += it->file_size(error); }
    return !error && files == PT5_DISC_PKG_INSTALLED_FILES && bytes == PT5_DISC_PKG_INSTALLED_BYTES;
}
static BOOL CALLBACK close_window(HWND window, LPARAM pid) {
    DWORD owner = 0; GetWindowThreadProcessId(window, &owner);
    if (owner == static_cast<DWORD>(pid) && IsWindowVisible(window) && !GetWindow(window, GW_OWNER))
        PostMessageW(window, WM_CLOSE, 0, 0);
    return TRUE;
}
// Opens RPCS3's installer, answers it, and closes RPCS3 once every file is in
// place. A player can also click Install themselves (PT5_RPCS3_AUTOCLICK=0).
static DWORD install_package(const fs::path& rpcs3, const std::wstring& args, const fs::path& game) {
    Handle job(INVALID_HANDLE_VALUE), process(INVALID_HANDLE_VALUE); DWORD pid = 0;
    start_child(rpcs3, args, rpcs3.parent_path(), L"install-package", job, process, &pid);
    Com<IUIAutomation> uia;
    if (env(L"PT5_RPCS3_AUTOCLICK") != L"0")
        CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(uia.put()));
    bool pressed = false, complete = false; int stable = 0;
    const ULONGLONG deadline = GetTickCount64() + 15ull * 60 * 1000;
    for (;;) {
        const DWORD waited = MsgWaitForMultipleObjects(1, &process.h, FALSE, 500, QS_ALLINPUT);
        if (waited == WAIT_OBJECT_0) break;
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        if (!pressed && uia.p) {
            pressed = press_install(uia.p, pid);
            if (pressed) setup_progress(0.07, "RPCS3 is installing the update from your disc");
        }
        stable = package_installed(game) ? stable + 1 : 0;
        if (stable >= 3) { complete = true; break; }
        if (GetTickCount64() > deadline) break;
    }
    if (complete) {
        EnumWindows(close_window, static_cast<LPARAM>(pid));
        if (WaitForSingleObject(process.h, 20000) != WAIT_OBJECT_0) TerminateJobObject(job.h, 0); // Only our RPCS3.
        return 0;
    }
    if (WaitForSingleObject(process.h, 0) != WAIT_OBJECT_0) {
        TerminateJobObject(job.h, 1);
        fail(L"RPCS3 did not finish installing the package from your disc within 15 minutes. See logs\\install-package.err, then run setup again.");
    }
    DWORD code = 1; GetExitCodeProcess(process.h, &code); return code;
}
static fs::path decrypt_executable(const fs::path& installed, const fs::path& temporary, const fs::path& rpcs3, bool managed = false) {
    const auto original = installed / L"USRDIR/EBOOT.BIN";
    if (!fs::is_regular_file(original)) fail(L"The installed update is missing USRDIR\\EBOOT.BIN. Select your decrypted update 1.04 ELF instead.");
    fs::copy_file(original, temporary / L"EBOOT.BIN");
    if (run(rpcs3, std::wstring(managed ? L"--allow-any-location " : L"") + L"--decrypt " + quote(temporary / L"EBOOT.BIN"),
            rpcs3.parent_path(), L"decrypt") != 0)
        fail(L"RPCS3 decryption failed. See logs\\decrypt.err, or use RPCS3's Decrypt PS3 Binaries menu and select the resulting ELF.");
    const auto result = temporary / L"EBOOT.elf";
    if (!fs::is_regular_file(result)) fail(L"RPCS3 did not produce EBOOT.elf. Decrypt update 1.04 in RPCS3 and select the ELF.");
    return result;
}
static fs::path installed_executable(const fs::path& installed) {
    for (const wchar_t* name : {L"EBOOT.elf", L"EBOOT.ELF", L"EMAIN.ELF"}) {
        const auto candidate = installed / L"USRDIR" / name;
        if (fs::is_regular_file(candidate) && hash(candidate) == PT5_ELF_SHA256) return candidate;
    }
    return {};
}
static fs::path executable_for(const fs::path& installed, const fs::path& temporary) {
    if (const auto found = installed_executable(installed); !found.empty()) return found;
    TASKDIALOG_BUTTON buttons[] = {{100,L"Select my decrypted 1.04 executable"}, {101,L"Decrypt it using my installed RPCS3"}};
    TASKDIALOGCONFIG td{}; td.cbSize = sizeof td;
    td.dwFlags = static_cast<TASKDIALOG_FLAGS>(TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION);
    td.pszWindowTitle = L"Project Tourline 5 - game executable";
    td.pszMainInstruction = L"Select the GT5 update 1.04 executable";
    td.pszContent = L"Choose the decrypted EBOOT.elf from your installed GT5 update 1.04, or let your installed RPCS3 decrypt a temporary copy of EBOOT.BIN. The original files are left unchanged. No game files, keys or RPCS3 are included.";
    td.cButtons = 2; td.pButtons = buttons; td.dwCommonButtons = TDCBF_CANCEL_BUTTON; brand(td);
    int choice = IDCANCEL;
    if (FAILED(TaskDialogIndirect(&td, &choice, nullptr, nullptr))) fail(L"Could not open executable setup.");
    if (choice == IDCANCEL) throw Cancelled{};
    if (choice == 100) return pick(false, L"Select decrypted GT5 BCUS98114 update 1.04 EBOOT.elf");
    auto rpcs3 = pick(false, L"Select your installed RPCS3", L"RPCS3 executable", L"rpcs3.exe");
    if (rpcs3.empty()) throw Cancelled{};
    return decrypt_executable(installed, temporary, rpcs3);
}
// Install the package supplied on the supported disc, without booting the game
// or asking the player to locate an update download. RPCS3 owns PKG decryption.
static fs::path disc_update(const fs::path& disc, const fs::path& rpcs3, bool managed = false) {
    if (_wcsicmp(rpcs3.filename().c_str(), L"rpcs3.exe") || !fs::is_regular_file(rpcs3))
        fail(L"Select your installed rpcs3.exe to prepare the game from your disc.");
    const auto root = rpcs3.parent_path();
    // A custom VFS can redirect installation to an unrelated game directory.
    // Leave it to the explicit existing-installation route, rather than guess.
    // Setup's own copy writes a default vfs.yml on its first start.
    if (!managed && (fs::exists(root/L"config/vfs.yml") || fs::exists(root/L"vfs.yml")))
        fail(L"This RPCS3 folder has a custom virtual-filesystem configuration. Import your existing 01.04 game-data folder, or use a separate RPCS3 folder with default paths for disc preparation.");
    const auto game = root/L"dev_hdd0/game/BCUS98114";
    // Never replace another version of the player's RPCS3 game data.
    if (fs::exists(game)) {
        if (!fs::is_regular_file(game/L"PARAM.SFO") ||
            sfo_value(game/L"PARAM.SFO", "TITLE_ID") != "BCUS98114" ||
            sfo_value(game/L"PARAM.SFO", "APP_VER") != "01.04")
            fail(L"This RPCS3 folder already contains another GT5 version or an incomplete installation. To keep it unchanged, use a separate RPCS3 folder for setup, or import an existing 01.04 installation.");
        return installed_folder(game);
    }
    const auto package = disc/L"INSDIR/DATA000.PKG";
    if (!fs::is_regular_file(package) || !fs::file_size(package))
        fail(L"Your disc dump is missing PS3_GAME\\INSDIR\\DATA000.PKG, which contains the required 01.04 files. Restore the INSDIR folder from your supported BCUS98114 v02.00 disc, or import an existing 01.04 installation. No separate update download is needed for this disc.");
    if (fs::file_size(package) != PT5_DISC_PKG_BYTES || hash(package) != PT5_DISC_PKG_SHA256)
        fail(L"The disc's DATA000.PKG does not match the supported BCUS98114 v02.00 package. Check your dump, or import matching installed 01.04 data. RPCS3 has not been started.");
    if (!silent_errors && !managed) {
        TASKDIALOGCONFIG td{}; td.cbSize = sizeof td;
        td.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
        td.pszWindowTitle = L"Project Tourline 5 - prepare from disc";
        td.pszMainInstruction = L"Install the files already on your disc";
        td.pszContent = L"Close any running RPCS3 window first. Setup will open your disc's bundled package in your RPCS3, press Install, and close RPCS3 when the files are in place. Leave its window alone until it closes. You do not need to boot GT5 or download an update.\n\nRPCS3 installs the package into its own game-data folder. Setup then imports a verified copy and decrypts a temporary copy of its executable.";
        td.dwCommonButtons = TDCBF_OK_BUTTON | TDCBF_CANCEL_BUTTON; brand(td);
        int choice = IDCANCEL;
        if (FAILED(TaskDialogIndirect(&td, &choice, nullptr, nullptr))) fail(L"Could not open disc preparation instructions.");
        if (choice != IDOK) throw Cancelled{};
    }
    setup_progress(0.06, "Installing the update from your disc with RPCS3 (its window opens and closes by itself)");
    if (install_package(rpcs3, std::wstring(managed ? L"--allow-any-location " : L"") + L"--installpkg " + quote(package), game) != 0)
        fail(L"RPCS3 could not install the package from your disc. See logs\\install-package.err. Close other RPCS3 windows and retry.");
    if (!fs::is_regular_file(game/L"PARAM.SFO"))
        fail(L"The disc package was not found in RPCS3's standard game-data folder after installation. Close other RPCS3 windows and retry; if RPCS3 asks, click Install and close RPCS3 when finished. If you use a custom dev_hdd0 location, select that installed 01.04 folder in the optional Installed data input instead.");
    return installed_folder(game);
}
static void configure(fs::path disc, fs::path installed, fs::path executable) {
    setup_progress(0.02, "Checking GT5 disc and update folders");
    if (disc.empty()) disc = pick(true, L"Select your extracted Gran Turismo 5 disc or PS3_GAME folder");
    if (disc.empty()) throw Cancelled{};
    disc = game_folder(disc);
    bool managed = false;
    if (installed.empty() && !easy_setup && executable.empty() && !silent_errors) {
        const bool automatic = managed_rpcs3_enabled();
        TASKDIALOG_BUTTON buttons[] = {
            {100, L"Prepare from my disc (recommended)\nNothing else to select. Setup downloads the official RPCS3 build (37 MB) only to unpack the update already on your disc, then removes it."},
            {102, L"Prepare from my disc with my installed RPCS3\nSelect rpcs3.exe. Setup presses its Install button and closes it for you."},
            {101, L"Import an existing 01.04 installation\nSelect installed GT5 data or an RPCS3 folder."}};
        TASKDIALOGCONFIG td{}; td.cbSize = sizeof td;
        td.dwFlags = static_cast<TASKDIALOG_FLAGS>(TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION);
        td.pszWindowTitle = L"Project Tourline 5 - game setup";
        td.pszMainInstruction = L"Prepare Gran Turismo 5";
        td.cButtons = automatic ? 3 : 2; td.pButtons = automatic ? buttons : buttons + 1;
        td.dwCommonButtons = TDCBF_CANCEL_BUTTON; brand(td);
        int choice = IDCANCEL;
        if (FAILED(TaskDialogIndirect(&td, &choice, nullptr, nullptr))) fail(L"Could not open game setup.");
        if (choice == IDCANCEL) throw Cancelled{};
        if (choice == 100) managed = true;
        else if (choice == 102) executable = pick(false, L"Select your installed RPCS3", L"RPCS3 executable", L"rpcs3.exe");
        else installed = pick(true, L"Select your GT5 1.04 installed data or RPCS3 folder");
        if (installed.empty() && executable.empty() && !managed) throw Cancelled{};
    }
    if (installed.empty()) {
        if (executable.empty()) {
            if (!managed_rpcs3_enabled())
                fail(L"Select rpcs3.exe in the RPCS3 / Executable input to prepare from your disc, or supply an existing 01.04 installation in Installed data. You do not need to find an update download.");
            managed = true;
            executable = managed_rpcs3();
        } else if (_wcsicmp(executable.filename().c_str(), L"rpcs3.exe"))
            fail(L"Select rpcs3.exe in the RPCS3 / Executable input to prepare from your disc, leave it empty to let setup fetch RPCS3, or supply an existing 01.04 installation in Installed data. You do not need to find an update download.");
        installed = disc_update(disc, executable, managed);
    }
    const auto selected_update = installed;
    installed = installed_folder(installed);
    Import staging;
    if (easy_setup || (!executable.empty() && _wcsicmp(executable.extension().c_str(), L".exe") == 0)) {
        setup_progress(0.10, "Checking the update executable");
        if (executable.empty()) executable = installed_executable(installed);
        if (executable.empty()) {
            // Only search the folder the player supplied and its standard RPCS3 layout.
            std::vector<fs::path> roots{selected_update};
            if (installed.parent_path().filename() == L"game" && installed.parent_path().parent_path().filename() == L"dev_hdd0")
                roots.push_back(installed.parent_path().parent_path().parent_path());
            for (const auto& folder : roots) {
                const auto candidate = folder/L"rpcs3.exe";
                if (fs::is_regular_file(candidate)) { executable = candidate; break; }
            }
        }
        if (executable.empty())
            fail(L"No matching decrypted update executable was found. Add your update 01.04 EBOOT.elf or installed rpcs3.exe to the optional Executable / RPCS3 input in Easy Setup, then retry.");
        if (_wcsicmp(executable.extension().c_str(), L".exe") == 0) {
            if (_wcsicmp(executable.filename().c_str(), L"rpcs3.exe") != 0 || !fs::is_regular_file(executable))
                fail(L"Select your installed rpcs3.exe or the decrypted GT5 update 01.04 EBOOT.elf.");
            setup_progress(0.12, managed ? "Decrypting a temporary copy with RPCS3" : "Decrypting a temporary copy with your RPCS3");
            executable = decrypt_executable(installed, staging.path, executable, managed);
        }
    } else if (executable.empty()) executable = executable_for(installed, staging.path);
    if (executable.empty()) throw Cancelled{};
    if (!fs::is_regular_file(executable) || fs::file_size(executable) != PT5_ELF_BYTES || hash(executable) != PT5_ELF_SHA256)
        fail(L"That executable does not match GT5 BCUS98114 update 01.04. The original disc executable and newer updates are different builds. Select the decrypted update 1.04 EBOOT.elf.");
    const fs::path game = data / L"gamedata/dev_hdd0/game/BCUS98114";
    if (within(staging.path, installed) || within(installed, staging.path) || within(data, installed))
        fail(L"Choose the source installation outside the launcher's import/storage root.");
    const bool reuse = fs::exists(game) && fs::equivalent(game, installed);
    setup_progress(0.20, "Copying and verifying installed update data");
    if (!reuse) copy_verified(installed, staging.path / L"game", L"Importing GT5 installed data");
    setup_progress(0.90, "Verifying imported executable");
    fs::copy_file(executable, staging.path / L"EMAIN.ELF");
    if (hash(staging.path / L"EMAIN.ELF") != PT5_ELF_SHA256) fail(L"The executable changed during import. Previous setup is unchanged.");
    fs::create_directories(game.parent_path()); fs::create_directories(data / L"input");
    const auto backup = data / L"backups" / unique_name(); const auto elf = data / L"input/EMAIN.ELF";
    bool old_game = false, old_elf = false, new_game = false, new_elf = false;
    setup_progress(0.95, "Activating verified GT5 setup");
    try {
        if (!reuse && fs::exists(game)) { fs::create_directories(backup); fs::rename(game, backup/L"game"); old_game = true; }
        if (!reuse) { fs::rename(staging.path/L"game", game); new_game = true; }
        if (fs::exists(elf)) { fs::create_directories(backup); fs::rename(elf, backup/L"EMAIN.ELF"); old_elf = true; }
        fs::rename(staging.path/L"EMAIN.ELF", elf); new_elf = true;
        atomic_ini(data/L"setup.ini", {{L"directory", disc}, {L"target", L"BCUS98114-v104"}});
    } catch (...) {
        if (new_elf) fs::rename(elf, staging.path/L"EMAIN.ELF");
        if (old_elf) fs::rename(backup/L"EMAIN.ELF", elf);
        if (new_game) fs::rename(game, staging.path/L"game");
        if (old_game) fs::rename(backup/L"game", game);
        throw;
    }
    // Setup's RPCS3 and its install have served their purpose; a failed setup keeps them for the retry.
    if (managed && env(L"PT5_KEEP_RPCS3") != L"1") {
        setup_progress(0.97, "Removing setup's temporary RPCS3");
        try { remove_tools(); } catch (...) {} // Leftovers are harmless and reused next time.
    }
}
static fs::path storage_picker() {
    TASKDIALOG_BUTTON buttons[] = {{100,L"Keep the current storage folder"}, {101,L"Choose a folder (including an external SSD)"}};
    TASKDIALOGCONFIG td{}; td.cbSize = sizeof td;
    td.dwFlags = static_cast<TASKDIALOG_FLAGS>(TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION);
    td.pszWindowTitle = L"Project Tourline 5 - storage location";
    td.pszMainInstruction = L"Where should Project Tourline 5 store its files?";
    const auto content = L"Saves, settings, shaders and an imported copy of the installed game data will be stored here:\n\n" + data.wstring() +
        L"\n\nYour disc dump stays where it is. The launcher stays where you extracted it. Changing storage copies existing data and keeps the original as a backup.";
    td.pszContent = content.c_str(); td.cButtons = 2; td.pButtons = buttons; td.dwCommonButtons = TDCBF_CANCEL_BUTTON; brand(td);
    int choice = IDCANCEL;
    if (FAILED(TaskDialogIndirect(&td, &choice, nullptr, nullptr))) fail(L"Could not open storage selection.");
    if (choice == IDCANCEL) throw Cancelled{};
    return choice == 100 ? data : pick(true, L"Choose an empty folder for Project Tourline 5 storage");
}
static void validate_setup() {
    if (profile(data/L"setup.ini", L"game", L"target") != L"BCUS98114-v104") fail(L"Setup is missing. Run tourline5.exe --setup.");
    game_folder(profile(data/L"setup.ini", L"game", L"directory"));
    installed_folder(data/L"gamedata/dev_hdd0/game/BCUS98114");
    const auto elf = data/L"input/EMAIN.ELF";
    if (!fs::is_regular_file(elf) || fs::file_size(elf) != PT5_ELF_BYTES || hash(elf) != PT5_ELF_SHA256)
        fail(L"The imported executable is missing or changed. Run tourline5.exe --setup.");
}
static fs::path validate_engine() {
    const auto release = install/L"pt5-release.ini";
    const auto engine = fs::is_regular_file(install/L"gt5.exe") ? install/L"gt5.exe" : install/L"runtime/gt5.exe";
    if (profile(release, L"game", L"target") != L"BCUS98114-v104" ||
        !fs::is_regular_file(engine) || hash(engine) != profile(release, L"game", L"engine_sha256"))
        fail(L"The launcher and engine do not match. Rebuild or re-extract the complete Project Tourline 5 player folder.");
    return engine;
}
static void report(const std::wstring& message) {
    fwprintf(stderr, L"%ls\n", message.c_str());
    try {
        if (!data.empty() && fs::is_directory(data)) {
            fs::create_directories(data/L"logs");
            const int n = WideCharToMultiByte(CP_UTF8, 0, message.c_str(), static_cast<int>(message.size()), nullptr, 0, nullptr, nullptr);
            std::string utf8(n, '\0');
            WideCharToMultiByte(CP_UTF8, 0, message.c_str(), static_cast<int>(message.size()), utf8.data(), n, nullptr, nullptr);
            std::ofstream(data/L"logs/setup.err") << utf8 << '\n';
        }
    } catch (...) {}
    if (!quiet && !silent_errors) MessageBoxW(nullptr, message.c_str(), L"Project Tourline 5", MB_OK|MB_ICONERROR);
}
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    struct Com { HRESULT result; ~Com() { if (SUCCEEDED(result)) CoUninitialize(); } } apartment{com};
    silent_errors = env(L"PT5_SETUP_UI") == L"0";
    try {
        bool force = false, open = false, change_storage = false, configure_only = false;
        fs::path storage, disc, installed, elf;
        int argc = 0; auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (!argv) fail(L"Could not read launcher options.");
        struct Arguments { LPWSTR* value; ~Arguments() { LocalFree(value); } } arguments{argv};
        for (int i = 1; i < argc; ++i) {
            const std::wstring arg = argv[i];
            if (arg == L"--setup") force = true;
            else if (arg == L"--recomplauncher-prepare") { easy_setup = true; force = true; configure_only = true; silent_errors = true; }
            else if (arg == L"--configure") { force = true; configure_only = true; silent_errors = true; }
            else if (arg == L"--check") { quiet = true; silent_errors = true; }
            else if (arg == L"--open-data") open = true;
            else if (arg == L"--choose-storage") change_storage = true;
            else if (arg.rfind(L"--executable=", 0) == 0) {
                if (arg.size() == 13) fail(L"The Executable / RPCS3 input is empty.");
                elf = fs::absolute(arg.substr(13));
            }
            else if (arg.rfind(L"--game-data=", 0) == 0) {
                if (arg.size() == 12) fail(L"The Installed data input is empty.");
                installed = fs::absolute(arg.substr(12));
            }
            else if ((arg == L"--set-storage" || arg == L"--game" || arg == L"--game-data" || arg == L"--elf") && i+1 < argc) {
                const fs::path value = fs::absolute(argv[++i]);
                if (arg == L"--set-storage") { storage = value; silent_errors = true; }
                else if (arg == L"--game") disc = value;
                else if (arg == L"--game-data") installed = value;
                else elf = value;
            } else fail(L"Supported options: --setup, --check, --open-data, --choose-storage, --set-storage <empty folder>, --configure --game <dump> [--elf <ELF or rpcs3.exe>] [--game-data <installation> --elf <ELF or rpcs3.exe>], --recomplauncher-prepare --game <dump> [--game-data=<installation>] [--executable=<ELF or rpcs3.exe>].");
        }
        if ((quiet && (force || open || change_storage || !storage.empty())) ||
            (open && (force || change_storage || !storage.empty())) ||
            (force && (change_storage || !storage.empty())) ||
            (change_storage && !storage.empty()) ||
            (configure_only && (disc.empty() || (!easy_setup && elf.empty() && !installed.empty()))) ||
            (!configure_only && (!disc.empty() || !installed.empty() || !elf.empty())))
            fail(L"Use --configure with --game (setup fetches RPCS3), optionally --elf, and --game-data with --elf when importing existing data, or --setup for the file pickers. --check cannot change setup or storage.");
        if (FAILED(com)) fail(L"Windows COM initialization failed.");
        std::array<wchar_t, 32768> module{};
        const DWORD length = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
        if (!length || length >= module.size()) fail(L"Could not locate the launcher.");
        install = fs::canonical(fs::path(module.data()).parent_path()); runtime_path(install);
        PWSTR local = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) fail(L"Could not locate Local AppData.");
        const auto home = fs::weakly_canonical(env(L"PT5_LAUNCHER_HOME").empty() ? fs::path(local)/L"ProjectTourline5" : fs::absolute(env(L"PT5_LAUNCHER_HOME")));
        CoTaskMemFree(local);
        const bool override_data = !env(L"PT5_USER_DIR").empty();
        data = override_data ? fs::weakly_canonical(fs::absolute(env(L"PT5_USER_DIR"))) : home;
        if (override_data && (change_storage || !storage.empty())) fail(L"Unset PT5_USER_DIR before changing remembered storage.");
        if (!override_data) {
            const auto saved = profile(home/L"storage.ini", L"game", L"directory");
            if (!saved.empty()) {
                data = saved;
                if (!data.is_absolute() || !fs::is_directory(data))
                    fail(L"Your selected Project Tourline 5 storage is unavailable:\n\n" + saved + L"\n\nReconnect the drive. Your saved selection has been kept.");
                data = fs::canonical(data);
            }
        }
        runtime_path(data);
        if (quiet) { validate_setup(); validate_engine(); return 0; }
        if (!open && !change_storage && storage.empty()) {
            validate_engine(); // Reject an incomplete/mismatched package before importing game data.
            if (!fs::is_directory(install/L"assets/glyphs")) fail(L"The player assets folder is missing. Rebuild or re-extract the complete player folder.");
        }
        fs::create_directories(home); fs::create_directories(data);
        if (open) {
            if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", data.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
                fail(L"Explorer could not open storage.");
            return 0;
        }
        Handle home_lock(CreateFileW((home/L"storage.lock").c_str(), GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, 0, nullptr));
        Handle data_lock(CreateFileW((data/L"launcher.lock").c_str(), GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, 0, nullptr));
        if (home_lock.h == INVALID_HANDLE_VALUE || data_lock.h == INVALID_HANDLE_VALUE)
            fail(L"Project Tourline 5 is already running, or this storage is not writable.");
        if (!override_data) link_storage(home);
        const bool first = !fs::exists(data/L"setup.ini") && !override_data && !configure_only && env(L"PT5_STORAGE_PICKER") != L"0" && !silent_errors;
        if (!override_data && (change_storage || !storage.empty() || first)) {
            auto selected = storage.empty() ? storage_picker() : storage;
            if (selected.empty()) throw Cancelled{};
            const auto before = data; relocate(selected, home);
            if (data != before) {
                CloseHandle(data_lock.h);
                data_lock.h = CreateFileW((data/L"launcher.lock").c_str(), GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, 0, nullptr);
                if (data_lock.h == INVALID_HANDLE_VALUE) fail(L"The selected storage is in use or not writable.");
            }
            link_storage(home);
            if (change_storage || !storage.empty()) return 0;
        }
        bool valid = false;
        if (!force) { try { validate_setup(); valid = true; } catch (const Failure&) {} catch (const fs::filesystem_error&) {} }
        if (!valid) {
            if (silent_errors && !configure_only) fail(L"Setup is missing or invalid. Run tourline5.exe --setup.");
            configure(disc, installed, elf);
        }
        if (configure_only) {
            if (easy_setup) { validate_setup(); setup_progress(1.0, "GT5 setup complete"); }
            return 0;
        }
        validate_setup(); const auto engine = validate_engine();
        if (!fs::is_directory(install/L"assets/glyphs")) fail(L"The player assets folder is missing. Rebuild or re-extract the complete player folder.");
        fs::create_directories(data/L"assets");
        fs::copy(install/L"assets/glyphs", data/L"assets/glyphs", fs::copy_options::recursive|fs::copy_options::overwrite_existing);
        const auto settings = data/L"gt5p_settings.toml";
        if (!fs::exists(settings)) {
            std::ofstream f(settings);
            // Keep in step with config.new in launcher/recomplauncher.json.
            f << "# Project Tourline 5 first-run preset. F1 opens settings.\n[video]\nresolution = \"720p\"\nmsaa = 2\nshader_compiler = \"dxc\"\nshader_warmup = true\n";
            if (!f) fail(L"Could not write initial settings.");
        }
        setenv(L"GT5P_ROOT", data.wstring()); setenv(L"PS3_VFS_ROOT", data.wstring());
        setenv(L"GT5P_SETTINGS", settings.wstring());
        const auto selected_game = game_folder(profile(data/L"setup.ini", L"game", L"directory"));
        setenv(L"GT5P_GAME_DIR", selected_game.wstring()+L"/");
        if (env(L"RSX_SHADER_CACHE").empty()) setenv(L"RSX_SHADER_CACHE", (data/L"shadercache").wstring());
        setenv(L"GT5P_LAUNCHER_RESTART", L"1");
        if (env(L"PS3_VERBOSE").empty()) setenv(L"PS3_VERBOSE", L"0");
        for (;;) {
            const auto code = run(engine, quote(data/L"input/EMAIN.ELF"), data, L"play");
            if (code == pt5_restart::exit_code) continue;
            if (offer_report(code)) { if (code) return 1; break; }
            if (code) fail(L"The game exited with code " + std::to_wstring(code) + L". Logs are in:\n\n" + (data/L"logs").wstring());
            break;
        }
        return 0;
    } catch (const Cancelled&) { return 0; }
      catch (const Failure& e) { report(e.message); }
      catch (const std::exception&) { report(L"A file operation failed. Check free space and folder access, then run tourline5.exe --setup again. Your original game files are unchanged."); }
    return 1;
}
