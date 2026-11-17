// Startup status. Between Play and the game window the launcher checks the
// engine, setup, installed data and disc, then starts the engine. That is
// normally about a second, but the first start after an update was reported
// to show nothing for about a minute (cause not reproduced on the
// developer's machine; antivirus scans of the new files are suspected). A
// small window with a moving bar names the current step once a start has
// taken 1.5 s, and closes when the game window is visible or the game exits.
// It hides while the launcher shows any other window (setup, disc check,
// save copy), so those need no changes. Every start appends each step's
// time to logs/launcher-start.txt (kept short; included in problem reports).
// PT5_START_STATUS=0: no window (the timing log is still written).
#pragma once

namespace start_status {
struct Step { std::wstring name; ULONGLONG ms; };
struct State {
    std::mutex mutex;
    std::wstring step;
    ULONGLONG start = 0, step_start = 0;
    std::vector<Step> steps;
    std::atomic<bool> done{false};
    HWND window = nullptr;
};
inline State* current = nullptr;
inline std::thread ui;

inline bool window_enabled() {
    wchar_t b[8]; const DWORD n = GetEnvironmentVariableW(L"PT5_START_STATUS", b, 8);
    return !(n && n < 8 && b[0] == L'0') && !silent_errors && !quiet;
}

// Another visible top-level window of this process (a launcher dialog).
inline bool other_window(HWND self) {
    struct Find { HWND self; bool found; } f{self, false};
    EnumWindows([](HWND h, LPARAM p) -> BOOL {
        auto& f = *reinterpret_cast<Find*>(p);
        DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
        if (pid == GetCurrentProcessId() && h != f.self && IsWindowVisible(h)) { f.found = true; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&f));
    return f.found;
}

inline std::wstring text(State& s) {
    std::lock_guard<std::mutex> guard(s.mutex);
    const auto now = GetTickCount64();
    std::wstring t = s.step + L"\n\n" + std::to_wstring((now - s.start) / 1000) + L" s";
    if (now - s.start >= 10000)
        t += L"\n\nThe first start after an update can take a minute or more, for example while antivirus "
             L"software checks the new game files. Later starts are quick.";
    return t;
}

inline HRESULT CALLBACK callback(HWND window, UINT notice, WPARAM, LPARAM, LONG_PTR data) {
    auto& s = *reinterpret_cast<State*>(data);
    if (notice == TDN_CREATED) {
        { std::lock_guard<std::mutex> guard(s.mutex); s.window = window; }
        SendMessageW(window, TDM_SET_PROGRESS_BAR_MARQUEE, TRUE, 30);
    }
    if (notice == TDN_TIMER) {
        if (s.done) { PostMessageW(window, WM_CLOSE, 0, 0); return S_OK; }
        ShowWindow(window, other_window(window) ? SW_HIDE : SW_SHOWNOACTIVATE);
        const auto t = text(s);
        SendMessageW(window, TDM_SET_ELEMENT_TEXT, TDE_CONTENT, reinterpret_cast<LPARAM>(t.c_str()));
    }
    return S_OK;
}

inline void show(State& s) {
    // Wait out a normal start before showing anything.
    while (!s.done && GetTickCount64() - s.start < 1500) Sleep(50);
    if (s.done) return;
    const auto first = text(s);
    TASKDIALOGCONFIG td{}; td.cbSize = sizeof td;
    td.dwFlags = static_cast<TASKDIALOG_FLAGS>(TDF_CALLBACK_TIMER | TDF_SHOW_MARQUEE_PROGRESS_BAR | TDF_ALLOW_DIALOG_CANCELLATION | TDF_CAN_BE_MINIMIZED);
    td.pszWindowTitle = L"Project Tourline 5";
    td.pszMainInstruction = L"Starting Gran Turismo 5";
    td.pszContent = first.c_str();
    td.dwCommonButtons = TDCBF_CLOSE_BUTTON;   // hides this window only; the start continues
    td.pfCallback = callback; td.lpCallbackData = reinterpret_cast<LONG_PTR>(&s);
    brand(td);
    TaskDialogIndirect(&td, nullptr, nullptr, nullptr);
}

inline void log(State& s, const wchar_t* outcome) {
    try {
        fs::create_directories(data/L"logs");
        const auto path = data/L"logs/launcher-start.txt";
        // Keep the last ~40 starts: drop the older half past 32 KiB.
        std::string old;
        { std::ifstream f(path, std::ios::binary); old.assign(std::istreambuf_iterator<char>(f), {}); }
        if (old.size() > 32768) { const auto cut = old.find('\n', old.size() / 2); old = cut == std::string::npos ? std::string{} : old.substr(cut + 1); }
        SYSTEMTIME t; GetLocalTime(&t);
        char head[96];
        snprintf(head, sizeof head, "%04u-%02u-%02u %02u:%02u:%02u launcher " PT5_VERSION " ",
                 t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        std::wstring line;
        ULONGLONG total = 0;
        for (const auto& step : s.steps) { line += L" | " + step.name + L" " + std::to_wstring(step.ms) + L" ms"; total += step.ms; }
        line = std::wstring(L"total ") + std::to_wstring(total) + L" ms, " + outcome + line;
        const int n = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), nullptr, 0, nullptr, nullptr);
        std::string utf8(n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), utf8.data(), n, nullptr, nullptr);
        std::ofstream(path, std::ios::binary | std::ios::trunc) << old << head << utf8 << '\n';
    } catch (...) {}
}

// Starts timing a start (and the window thread). Ends any previous one.
inline void begin(const wchar_t* step);
inline void end(const wchar_t* outcome) {
    State* s = current;
    if (!s) return;
    {
        std::lock_guard<std::mutex> guard(s->mutex);
        s->steps.push_back({s->step, GetTickCount64() - s->step_start});
    }
    s->done = true;
    if (ui.joinable()) ui.join();
    log(*s, outcome);
    current = nullptr;
    delete s;
}
inline void begin(const wchar_t* step) {
    end(L"replaced");
    auto* s = new State;
    s->start = s->step_start = GetTickCount64();
    s->step = step;
    current = s;
    if (window_enabled()) ui = std::thread([s] { show(*s); });
}
// The next step of the current start.
inline void step(const wchar_t* name) {
    State* s = current;
    if (!s) return;
    std::lock_guard<std::mutex> guard(s->mutex);
    const auto now = GetTickCount64();
    s->steps.push_back({s->step, now - s->step_start});
    s->step = name; s->step_start = now;
}
inline bool active() { return current != nullptr; }

// A visible top-level window of process pid: the game's window is up.
inline bool has_window(DWORD pid) {
    struct Find { DWORD pid; bool found; } f{pid, false};
    EnumWindows([](HWND h, LPARAM p) -> BOOL {
        auto& f = *reinterpret_cast<Find*>(p);
        DWORD owner = 0; GetWindowThreadProcessId(h, &owner);
        if (owner == f.pid && IsWindowVisible(h)) { f.found = true; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&f));
    return f.found;
}
}  // namespace start_status
