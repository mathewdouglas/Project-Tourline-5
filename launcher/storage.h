// File operations shared by interactive and command-line setup. Adapted from
// Tourline Prologue, with staged imports and cancellable verified copying.
#pragma once
struct Cancelled {};
static bool within(const fs::path& a, const fs::path& b) {
    auto x = a.lexically_normal().wstring(), y = b.lexically_normal().wstring();
    std::transform(x.begin(), x.end(), x.begin(), towlower);
    std::transform(y.begin(), y.end(), y.begin(), towlower);
    if (x == y) return true;
    if (!y.empty() && y.back() != L'\\') y += L'\\';
    return x.compare(0, y.size(), y) == 0;
}
static std::wstring unique_name() {
    return std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
}
static void atomic_ini(const fs::path& path, const std::vector<std::pair<std::wstring, fs::path>>& values) {
    const fs::path staged = path.wstring() + L".new-" + unique_name();
    {
        std::ofstream f(staged, std::ios::binary | std::ios::trunc); f.write("\xff\xfe", 2);
        if (!f) fail(L"Could not create configuration: " + staged.wstring());
    }
    for (const auto& [key, value] : values) setting(staged, key.c_str(), value);
    // A cache flush returns zero even on success; verify the written values.
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, staged.c_str());
    for (const auto& [key, value] : values)
        if (profile(staged, L"game", key.c_str()) != value.wstring()) fail(L"Could not verify saved configuration.");
    if (!MoveFileExW(staged.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        fail(L"Could not save configuration. The previous selection is unchanged: " + path.wstring());
}
struct CopyItem { fs::path relative; uintmax_t bytes; bool directory; };
struct CopyState {
    std::atomic<bool> cancel{false}, done{false};
    std::atomic<uint64_t> copied{0};
    uint64_t total = 0, completed = 0;
    ULONGLONG last_progress_ms = 0;
    std::mutex mutex;
    std::wstring current;
    std::exception_ptr error;
};
static DWORD CALLBACK copy_progress(LARGE_INTEGER, LARGE_INTEGER transferred, LARGE_INTEGER,
    LARGE_INTEGER, DWORD, DWORD, HANDLE, HANDLE, LPVOID parameter) {
    auto& state = *static_cast<CopyState*>(parameter);
    state.copied = state.completed + static_cast<uint64_t>(transferred.QuadPart);
    const auto now = GetTickCount64();
    if (easy_setup && now - state.last_progress_ms >= 250) {
        state.last_progress_ms = now;
        setup_progress(0.20 + 0.65 * state.copied / std::max<uint64_t>(state.total, 1),
                       "Copying and verifying installed update data");
    }
    return state.cancel ? PROGRESS_CANCEL : PROGRESS_CONTINUE;
}
static HRESULT CALLBACK copy_dialog(HWND window, UINT notice, WPARAM button, LPARAM, LONG_PTR parameter) {
    auto& state = *reinterpret_cast<CopyState*>(parameter);
    if (notice == TDN_CREATED) SendMessageW(window, TDM_SET_PROGRESS_BAR_RANGE, 0, MAKELPARAM(0, 1000));
    if (notice == TDN_BUTTON_CLICKED && button == IDCANCEL && !state.done) {
        state.cancel = true;
        SendMessageW(window, TDM_SET_ELEMENT_TEXT, TDE_CONTENT, (LPARAM)L"Cancelling after the current file operation. Your original files are unchanged.");
        return S_FALSE;
    }
    if (notice == TDN_TIMER) {
        if (state.done) { SendMessageW(window, TDM_CLICK_BUTTON, IDCANCEL, 0); return S_OK; }
        SendMessageW(window, TDM_SET_PROGRESS_BAR_POS, static_cast<WPARAM>(1000.0 * state.copied / std::max<uint64_t>(state.total, 1)), 0);
        if (!state.cancel) {
            std::lock_guard<std::mutex> guard(state.mutex);
            SendMessageW(window, TDM_SET_ELEMENT_TEXT, TDE_CONTENT, reinterpret_cast<LPARAM>(state.current.c_str()));
        }
    }
    return S_OK;
}
// Runs work on a helper thread behind a cancellable progress dialog (inline when
// setup has no UI). Work reports through state.copied/total/current.
static void with_progress(const wchar_t* title, const wchar_t* content, CopyState& state, const std::function<void()>& body) {
    auto work = [&] {
        try { body(); } catch (...) { state.error = std::current_exception(); }
        state.done = true;
    };
    if (silent_errors || quiet) work();
    else {
        std::thread worker(work);
        TASKDIALOGCONFIG td{}; td.cbSize = sizeof td;
        td.dwFlags = static_cast<TASKDIALOG_FLAGS>(TDF_CALLBACK_TIMER | TDF_SHOW_PROGRESS_BAR | TDF_ALLOW_DIALOG_CANCELLATION);
        td.pszWindowTitle = L"Project Tourline 5"; td.pszMainInstruction = title;
        td.pszContent = content;
        td.dwCommonButtons = TDCBF_CANCEL_BUTTON; td.pfCallback = copy_dialog; td.lpCallbackData = reinterpret_cast<LONG_PTR>(&state); brand(td);
        const HRESULT shown = TaskDialogIndirect(&td, nullptr, nullptr, nullptr);
        if (FAILED(shown)) state.cancel = true;
        worker.join();
        if (FAILED(shown)) fail(L"Could not display progress. Your previous setup remains selected.");
    }
    if (state.error) std::rethrow_exception(state.error);
}
// Always copy into a new staging/empty directory. Never move a source file.
static void copy_verified(const fs::path& source, const fs::path& destination, const wchar_t* title,
                          bool storage_copy = false) {
    std::vector<CopyItem> files; CopyState state;
    for (const auto& entry : fs::recursive_directory_iterator(source)) {
        const auto relative = entry.path().lexically_relative(source);
        if (storage_copy && (relative == L"launcher.lock" || relative == L"storage.lock" || relative == L"storage.ini")) continue;
        if (GetFileAttributesW(entry.path().c_str()) & FILE_ATTRIBUTE_REPARSE_POINT)
            fail(L"A source contains a linked file or folder. Copying stopped without changing the source:\n\n" + entry.path().wstring());
        if (!entry.is_directory() && !entry.is_regular_file()) fail(L"Unsupported source file: " + entry.path().wstring());
        const auto bytes = entry.is_regular_file() ? entry.file_size() : 0;
        if (bytes > UINT64_MAX - state.total) fail(L"The selected data is too large.");
        files.push_back({relative, bytes, entry.is_directory()}); state.total += bytes;
    }
    fs::create_directories(destination);
    if (fs::space(destination).available < state.total + 64 * 1024 * 1024)
        fail(L"Not enough free space in " + destination.wstring() + L". The source remains unchanged.");
    with_progress(title, L"Copying your selected files. Your original files will not be modified.", state, [&] {
        for (const auto& item : files) {
            if (state.cancel) throw Cancelled{};
            const auto from = source / item.relative, to = destination / item.relative;
            if (item.directory) { fs::create_directories(to); continue; }
            fs::create_directories(to.parent_path());
            {
                std::lock_guard<std::mutex> guard(state.mutex);
                state.current = L"Copying and verifying " + item.relative.wstring() +
                    L"\n\nYour source files stay in place. The previous setup remains available until this completes.";
            }
            if (!CopyFileExW(from.c_str(), to.c_str(), copy_progress, &state, nullptr, COPY_FILE_FAIL_IF_EXISTS)) {
                if (state.cancel) throw Cancelled{};
                fail(L"Could not copy " + from.wstring() + L". The source and previous setup are unchanged.");
            }
            if (hash(from) != hash(to)) fail(L"Copy verification failed: " + to.wstring());
            state.completed += item.bytes; state.copied = state.completed;
        }
        if (state.cancel) throw Cancelled{};
    });
}
// A launcher-owned temporary directory, confined to this storage's imports.
struct Import {
    fs::path path, parent;
    Import() {
        parent = fs::weakly_canonical(data / L"imports"); fs::create_directories(parent);
        path = parent / (L"setup-" + unique_name());
        if (!fs::create_directory(path)) fail(L"Could not create fresh setup staging.");
    }
    ~Import() {
        try {
            const auto resolved = fs::weakly_canonical(path);
            if (resolved != path || resolved.parent_path() != parent || !within(parent, fs::weakly_canonical(data))) return;
            for (const auto& entry : fs::recursive_directory_iterator(path))
                if (GetFileAttributesW(entry.path().c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) return;
            fs::remove_all(path);
        } catch (...) {} // Leave an unreferenced import if safe cleanup is unavailable.
    }
};
static void relocate(const fs::path& chosen, const fs::path& home) {
    auto destination = fs::weakly_canonical(fs::absolute(chosen));
    const auto source = fs::weakly_canonical(data); runtime_path(destination);
    if (within(destination, source) && within(source, destination)) return;
    if (within(destination, source) || within(source, destination) || within(install, destination) || within(home, destination))
        fail(L"Choose a separate storage folder outside current storage, and not a folder containing the launcher or its configuration.");
    if (fs::exists(destination) && (!fs::is_directory(destination) || !fs::is_empty(destination)))
        fail(L"Choose an empty storage folder. Existing files will not be overwritten.");
    fs::create_directories(destination);
    Handle lock(CreateFileW((destination/L"launcher.lock").c_str(), GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, 0, nullptr));
    if (lock.h == INVALID_HANDLE_VALUE) fail(L"The new storage is not writable or is already in use.");
    copy_verified(source, destination, L"Copying Project Tourline 5 storage", true);
    atomic_ini(home/L"storage.ini", {{L"directory", destination}});
    data = destination; // Preserve old storage as a recovery copy.
}
