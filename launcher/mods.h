// Data mods for the selected game version (GT5 Master Mod and the like).
//
// A mod lives in <version storage>/mods/<id>/: its PDIPFS folder, optional
// add-ons under addons/<name>/PDIPFS, and mod.ini. Only PDIPFS trees are taken
// from a mod's ZIP; nothing else in it is kept or run. The runtime reads them
// over the installed data (PS3_FS_OVERLAY, cellFs.c), so the installed data is
// never changed and Off is the unmodded game. A ZIP that carries the files it
// replaces (PDIPFS_ORIGINAL, as Master Mod does) is installed only when those
// match this installation: a mod made for another version or region is refused.
// Turning a mod on from Off first copies the version's save to backups/.
//
// The engine shows the choices in F1 > Game > Mod (GT5P_MOD_CHOICES,
// GT5P_MOD_ACTIVE) and asks for a change or the manager by exit code
// (pt5_restart::mod_select | N, pt5_restart::mods_manage).
// PT5_MODS=0: no mod support (nothing loaded, no F1 row). A PS3_FS_OVERLAY set
// before the launcher starts wins over the selection.
#pragma once
namespace mods {

inline bool enabled() { return env(L"PT5_MODS") != L"0"; }
inline fs::path root() { return target_data() / L"mods"; }
inline fs::path selection_ini() { return root() / L"mods.ini"; }

struct Mod { std::wstring id, name; std::vector<std::wstring> addons; };
struct Choice { std::wstring id, addon, label; };   // id empty: Off

// Deletes a launcher-owned folder. Links inside it are removed as links,
// never followed.
static void remove_tree(const fs::path& dir) {
    std::error_code ec;
    const DWORD top = GetFileAttributesW(dir.c_str());
    if (top == INVALID_FILE_ATTRIBUTES) return;
    if (top & FILE_ATTRIBUTE_REPARSE_POINT) { RemoveDirectoryW(dir.c_str()) || DeleteFileW(dir.c_str()); return; }
    std::vector<fs::path> links;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        if (GetFileAttributesW(it->path().c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) {
            links.push_back(it->path());
            it.disable_recursion_pending();
        }
    for (const auto& link : links)
        if (!RemoveDirectoryW(link.c_str()) && !DeleteFileW(link.c_str()))
            fail(L"Could not remove a linked item from a mod folder: " + link.wstring());
    fs::remove_all(dir);
}

// Printable ASCII, so the name reaches the engine's ANSI environment intact.
static std::wstring ascii(const std::wstring& s) {
    std::wstring out;
    for (wchar_t c : s) out += (c >= 0x20 && c < 0x7F && c != L'|') ? c : L' ';
    return out;
}
static std::wstring pretty(std::wstring s) {
    std::replace(s.begin(), s.end(), L'_', L' ');
    s = ascii(s);
    std::wstring out;
    for (wchar_t c : s) if (!(c == L' ' && (out.empty() || out.back() == L' '))) out += c;
    while (!out.empty() && out.back() == L' ') out.pop_back();
    return out;
}
// "Debug_Cam_Addon" -> "Debug Cam": the page's value box is narrow.
static std::wstring addon_label(const std::wstring& folder) {
    std::wstring s = pretty(folder);
    for (const wchar_t* tail : {L" Addon", L" Add-on", L" Add on", L" addon", L" add-on"}) {
        const size_t n = wcslen(tail);
        if (s.size() > n && s.compare(s.size() - n, n, tail) == 0) { s.resize(s.size() - n); break; }
    }
    return s;
}
// "GT5_2.11_Master_Mod_2.0.0_(US-BCUS98114)" -> "Master Mod 2.0.0": the game
// name, its version and a trailing region tag say nothing the page does not.
static std::wstring display_name(const std::wstring& stem) {
    std::wstring s = pretty(stem);
    const auto paren = s.rfind(L" (");
    if (paren != std::wstring::npos && s.back() == L')') s.erase(paren);
    auto drop = [&](const std::wstring& word) {
        if (s.size() > word.size() + 1 && s.compare(0, word.size() + 1, word + L" ") == 0) { s.erase(0, word.size() + 1); return true; }
        return false;
    };
    drop(L"GT5"); drop(L"2.11") || drop(L"02.11") || drop(L"1.04") || drop(L"01.04");
    return s.empty() ? pretty(stem) : s;
}
static std::wstring make_id(const std::wstring& name) {
    std::wstring id;
    for (wchar_t c : name) {
        const wchar_t l = towlower(c);
        if ((l >= L'a' && l <= L'z') || (l >= L'0' && l <= L'9') || l == L'.') id += l;
        else if (!id.empty() && id.back() != L'-') id += L'-';
    }
    while (!id.empty() && (id.back() == L'-' || id.back() == L'.')) id.pop_back();
    if (id.size() > 48) id.resize(48);
    return id.empty() ? L"mod" : id;
}

static std::vector<Mod> list() {
    std::vector<Mod> out;
    std::error_code ec;
    if (!fs::is_directory(root(), ec)) return out;
    for (const auto& e : fs::directory_iterator(root(), ec)) {
        const auto dir = e.path();
        if (!e.is_directory() || dir.filename().wstring()[0] == L'.' ||
            !fs::is_regular_file(dir / L"mod.ini") || !fs::is_directory(dir / L"PDIPFS")) continue;
        Mod m{dir.filename().wstring(), ascii(profile(dir / L"mod.ini", L"game", L"name")), {}};
        if (m.name.empty()) m.name = m.id;
        std::error_code ec2;
        if (fs::is_directory(dir / L"addons", ec2))
            for (const auto& a : fs::directory_iterator(dir / L"addons", ec2))
                if (a.is_directory() && fs::is_directory(a.path() / L"PDIPFS")) m.addons.push_back(a.path().filename().wstring());
        std::sort(m.addons.begin(), m.addons.end());
        out.push_back(m);
    }
    std::sort(out.begin(), out.end(), [](const Mod& a, const Mod& b) { return a.id < b.id; });
    return out;
}
// Off, each mod, then each mod with one of its add-ons.
static std::vector<Choice> choices() {
    std::vector<Choice> c{{L"", L"", L"Off"}};
    for (const auto& m : list()) {
        c.push_back({m.id, L"", m.name});
        for (const auto& a : m.addons) {
            // The page's value box holds about 26 characters: drop the mod's
            // version number from the pair first ("Master Mod + Debug Cam").
            std::wstring base = m.name;
            const auto space = base.rfind(L' ');
            if ((base + L" + " + addon_label(a)).size() > 26 && space != std::wstring::npos && iswdigit(base[space + 1]))
                base.erase(space);
            c.push_back({m.id, a, base + L" + " + addon_label(a)});
        }
    }
    if (c.size() > 255) c.resize(255);   // the exit code carries the index in 8 bits
    return c;
}
static size_t active_index(const std::vector<Choice>& c) {
    const auto id = profile(selection_ini(), L"game", L"active");
    const auto addon = profile(selection_ini(), L"game", L"addon");
    for (size_t i = 0; i < c.size(); ++i)
        if (c[i].id == id && c[i].addon == addon) return i;
    return 0;
}

// Copies this version's save to backups/save-before-mod-<time> (a mod can add
// cars or events the unmodded game does not know).
static void backup_save() {
    const auto home = target_data() / L"gamedata/dev_hdd0/home";
    std::error_code ec;
    if (!fs::is_directory(home, ec)) return;
    const auto backup = data / L"backups" / (L"save-before-mod-" + unique_name());
    for (const auto& user : fs::directory_iterator(home, ec))
        for (const wchar_t* dir : {L"BCUS98114-GAME", L"BCUS98114-BKUP"}) {
            const auto from = user.path() / L"savedata" / dir;
            if (!fs::is_directory(from)) continue;
            fs::create_directories(backup / user.path().filename());
            fs::copy(from, backup / user.path().filename() / dir, fs::copy_options::recursive);
        }
}
static void select(size_t index) {
    const auto c = choices();
    if (index >= c.size()) fail(L"That mod is no longer installed.");
    if (!c[index].id.empty() && c[active_index(c)].id.empty()) backup_save();
    fs::create_directories(root());
    atomic_ini(selection_ini(), {{L"active", c[index].id}, {L"addon", c[index].addon}});
}

// The engine's environment for the next start.
static void apply_environment(const std::wstring& user_overlay) {
    std::wstring overlay = user_overlay, labels;
    size_t active = 0;
    if (enabled()) {
        const auto c = choices();
        active = active_index(c);
        for (size_t i = 0; i < c.size(); ++i) labels += (i ? L"|" : L"") + c[i].label;
        if (user_overlay.empty() && !c[active].id.empty()) {
            const auto dir = root() / c[active].id;
            overlay = c[active].addon.empty() ? dir.wstring() : (dir / L"addons" / c[active].addon).wstring() + L";" + dir.wstring();
            runtime_path(dir);
        }
    }
    SetEnvironmentVariableW(L"PS3_FS_OVERLAY", overlay.empty() ? nullptr : overlay.c_str());
    SetEnvironmentVariableW(L"GT5P_MOD_CHOICES", labels.empty() ? nullptr : labels.c_str());
    setenv(L"GT5P_MOD_ACTIVE", std::to_wstring(active));
}

static uint64_t tree_bytes(const fs::path& dir) {
    uint64_t n = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec)) n += it->file_size(ec);
    return n;
}
static fs::path tar_exe() {
    std::array<wchar_t, MAX_PATH> system{};
    const UINT length = GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size()));
    const fs::path tar = length ? fs::path(system.data()) / L"tar.exe" : fs::path{};
    if (tar.empty() || !fs::is_regular_file(tar)) fail(L"Windows' tar.exe is missing; installing a mod needs Windows 10 (version 1803) or later.");
    return tar;
}
static bool same_bytes(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    if (fs::file_size(a, ec) != fs::file_size(b, ec) || ec) return false;
    std::ifstream x(a, std::ios::binary), y(b, std::ios::binary);
    return std::equal(std::istreambuf_iterator<char>(x), std::istreambuf_iterator<char>(),
                      std::istreambuf_iterator<char>(y), std::istreambuf_iterator<char>());
}

// Installs a mod ZIP for the selected version and selects it. Returns its name.
static std::wstring install(const fs::path& zip) {
    if (!fs::is_regular_file(zip)) fail(L"The mod file was not found: " + zip.wstring());
    const auto installed = target_data() / L"gamedata/dev_hdd0/game/BCUS98114/USRDIR/PDIPFS";
    if (!fs::is_directory(installed)) fail(L"GT5 " + std::wstring(target->version) + L" is not set up in this storage.");
    const auto sha = hash(zip);
    for (const auto& m : list())
        if (profile(root() / m.id / L"mod.ini", L"game", L"sha256") == sha)
            fail(L"This mod is already installed as \"" + m.name + L"\". Choose it in F1 > Game > Mod.");
    fs::create_directories(root());
    const auto zip_bytes = fs::file_size(zip);
    if (fs::space(root()).available < zip_bytes + zip_bytes / 4 + 64ull * 1024 * 1024)
        fail(L"Not enough free space in " + root().wstring() + L" to install this mod.");
    const auto staging = root() / (L".unpack-" + unique_name());
    const auto final_dir = root() / (L".new-" + unique_name());
    struct Cleanup {
        fs::path a, b;
        ~Cleanup() { try { remove_tree(a); remove_tree(b); } catch (...) {} }
    } cleanup{staging, final_dir};
    fs::create_directories(staging);

    CopyState state; state.total = std::max<uint64_t>(zip_bytes, 1);
    with_progress(L"Installing a mod", L"Unpacking the mod. Your installed game data is not changed.", state, [&] {
        {
            std::lock_guard<std::mutex> guard(state.mutex);
            state.current = L"Unpacking " + zip.filename().wstring() + L". Your installed game data is not changed.";
        }
        Handle job(INVALID_HANDLE_VALUE), process(INVALID_HANDLE_VALUE);
        start_child(tar_exe(), L"-xf " + quote(zip) + L" -C " + quote(staging), staging, L"unpack-mod", job, process);
        while (WaitForSingleObject(process.h, 250) == WAIT_TIMEOUT) {
            if (state.cancel) {
                TerminateJobObject(job.h, 1);
                WaitForSingleObject(process.h, 10000);
                throw Cancelled{};
            }
            state.copied = std::min<uint64_t>(tree_bytes(staging), state.total - 1);
        }
        DWORD code = 1; GetExitCodeProcess(process.h, &code);
        if (code) fail(L"The mod's ZIP could not be unpacked (logs\\unpack-mod.err). Nothing was installed.");
        state.copied = state.total;
    });

    // The mod's files: nothing in it may link elsewhere.
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(staging, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        if (GetFileAttributesW(it->path().c_str()) & FILE_ATTRIBUTE_REPARSE_POINT)
            fail(L"The mod's ZIP contains a link (" + it->path().filename().wstring() + L"). Nothing was installed.");
    // Its PDIPFS folder sits at the top or inside one wrapping folder.
    fs::path top = staging;
    if (!fs::is_directory(top / L"PDIPFS")) {
        std::vector<fs::path> dirs;
        for (const auto& e : fs::directory_iterator(staging)) if (e.is_directory()) dirs.push_back(e.path());
        if (dirs.size() == 1 && fs::is_directory(dirs[0] / L"PDIPFS")) top = dirs[0];
    }
    if (!fs::is_directory(top / L"PDIPFS") || fs::is_empty(top / L"PDIPFS"))
        fail(L"This ZIP has no PDIPFS folder, so it is not a GT5 data mod this launcher can install. Nothing was installed.");
    // Files the mod replaces, when it says which: they must be this installation's.
    const auto original = top / L"PDIPFS_ORIGINAL";
    if (fs::is_directory(original)) {
        size_t compared = 0;
        for (const auto& e : fs::recursive_directory_iterator(original)) {
            if (!e.is_regular_file()) continue;
            const auto mine = installed / e.path().lexically_relative(original);
            if (fs::is_regular_file(mine) && volmap::is_record(mine)) continue;   // read from the disc; not compared
            if (!fs::is_regular_file(mine) || !same_bytes(e.path(), mine))
                fail(L"This mod was made for a different game version or region than your GT5 " + std::wstring(target->version) +
                     L" (" + e.path().lexically_relative(original).wstring() + L" differs). Nothing was installed.");
            ++compared;
        }
        if (!compared) fail(L"The mod's original files could not be checked against your installation. Nothing was installed.");
    }

    const auto name = display_name(zip.stem().wstring());
    std::wstring id = make_id(name);
    for (int n = 2; fs::exists(root() / id); ++n) id = make_id(name) + L"-" + std::to_wstring(n);
    fs::create_directories(final_dir);
    fs::rename(top / L"PDIPFS", final_dir / L"PDIPFS");
    std::vector<std::wstring> addons;
    for (const auto& e : fs::directory_iterator(top)) {
        const auto folder = e.path().filename().wstring();
        if (!e.is_directory() || folder == L"PDIPFS_ORIGINAL" || !fs::is_directory(e.path() / L"PDIPFS")) continue;
        std::wstring addon;   // a folder name that stays readable on the page ("Debug Cam Addon")
        for (wchar_t ch : folder)
            addon += (iswalnum(ch) && ch < 0x80) || ch == L'-' || ch == L'.' ? ch : L'_';
        if (addon.empty() || addon[0] == L'.' || fs::exists(final_dir / L"addons" / addon)) continue;
        fs::create_directories(final_dir / L"addons" / addon);
        fs::rename(e.path() / L"PDIPFS", final_dir / L"addons" / addon / L"PDIPFS");
        addons.push_back(pretty(folder));
    }
    std::wstring addon_list;
    for (const auto& a : addons) addon_list += (addon_list.empty() ? L"" : L", ") + a;
    atomic_ini(final_dir / L"mod.ini", {{L"name", name}, {L"source", zip.filename()}, {L"sha256", sha}, {L"addons", addon_list}});
    fs::rename(final_dir, root() / id);
    const auto c = choices();
    for (size_t i = 0; i < c.size(); ++i)
        if (c[i].id == id && c[i].addon.empty()) { select(i); break; }
    fwprintf(stderr, L"[mods] installed %ls as %ls (%llu bytes)\n", zip.filename().c_str(), id.c_str(),
             static_cast<unsigned long long>(tree_bytes(root() / id)));
    return name;
}

static void remove(const std::wstring& id) {
    bool found = false;
    for (const auto& m : list()) found |= m.id == id;
    if (!found) fail(L"No installed mod is called \"" + id + L"\".");
    if (profile(selection_ini(), L"game", L"active") == id) select(0);
    remove_tree(root() / id);
}

// F1 > Game > Mods: install from a ZIP, or remove, until closed.
static void manage() {
    for (;;) {
        const auto installed_mods = list();
        const auto c = choices();
        std::vector<std::wstring> texts;
        std::vector<TASKDIALOG_BUTTON> buttons;
        texts.push_back(L"Install a mod from a ZIP file\nFor example GT5 Master Mod for 2.11. Your installed game data is not changed; Off in F1 is the unmodded game.");
        for (const auto& m : installed_mods)
            texts.push_back(L"Remove " + m.name + L"\nDeletes its files from your storage. Your save is not changed.");
        for (size_t i = 0; i < texts.size() && i < 30; ++i) buttons.push_back({static_cast<int>(100 + i), texts[i].c_str()});
        std::wstring content = L"Mods for GT5 " + std::wstring(target->version) + L". Choose which one loads in F1 > Game > Mod.\n\nInstalled: ";
        if (installed_mods.empty()) content += L"none.";
        for (size_t i = 0; i < installed_mods.size(); ++i) content += (i ? L", " : L"") + installed_mods[i].name;
        content += L"\nLoaded at start: " + c[active_index(c)].label + L".\n\nTurning a mod on keeps a copy of your save in the backups folder first. "
                   L"Progress made with a mod stays in this version's save.";
        TASKDIALOGCONFIG td{}; td.cbSize = sizeof td;
        td.dwFlags = static_cast<TASKDIALOG_FLAGS>(TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION);
        td.pszWindowTitle = L"Project Tourline 5 - mods";
        td.pszMainInstruction = L"Game mods";
        td.pszContent = content.c_str();
        td.pButtons = buttons.data(); td.cButtons = static_cast<UINT>(buttons.size());
        td.dwCommonButtons = TDCBF_CLOSE_BUTTON; brand(td);
        int choice = IDCLOSE;
        if (FAILED(TaskDialogIndirect(&td, &choice, nullptr, nullptr))) fail(L"Could not open the mod manager.");
        if (choice < 100) return;
        try {
            if (choice == 100) {
                const auto zip = pick(false, L"Choose a mod ZIP file", L"Mod archive (ZIP)", L"*.zip");
                if (zip.empty()) continue;
                const auto name = install(zip);
                MessageBoxW(nullptr, (name + L" is installed and loads when the game starts. F1 > Game > Mod turns it off or picks another.").c_str(),
                            L"Project Tourline 5 - mods", MB_OK | MB_ICONINFORMATION);
            } else {
                const auto& m = installed_mods.at(static_cast<size_t>(choice - 101));
                if (MessageBoxW(nullptr, (L"Remove " + m.name + L"?\n\nIf your save has cars or events from it, keep the mod or restore the save copied to the backups folder when it was turned on.").c_str(),
                                L"Project Tourline 5 - mods", MB_OKCANCEL | MB_ICONQUESTION) == IDOK)
                    remove(m.id);
            }
        } catch (const Cancelled&) {
        } catch (const Failure& f) {
            MessageBoxW(nullptr, f.message.c_str(), L"Project Tourline 5 - mods", MB_OK | MB_ICONERROR);
        } catch (const fs::filesystem_error&) {
            MessageBoxW(nullptr, L"A mod file operation failed. Check free space and folder access. Your installed game data is unchanged.",
                        L"Project Tourline 5 - mods", MB_OK | MB_ICONERROR);
        }
    }
}

}  // namespace mods
