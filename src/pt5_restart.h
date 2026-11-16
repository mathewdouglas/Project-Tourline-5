#pragma once
#include <cstdlib>
#include <cstring>
namespace pt5_restart {
constexpr unsigned long exit_code = 0x50543552;
constexpr unsigned long version_104 = 0x50543104;
constexpr unsigned long version_211 = 0x50543211;
constexpr unsigned long setup_211 = 0x50543521;
// F1 > Game > Mods: the launcher's mod manager (install, remove), then the game again.
constexpr unsigned long mods_manage = 0x50543522;
// F1 > Game > Mod: load mod choice N (GT5P_MOD_CHOICES order, 0 = Off) from now on.
constexpr unsigned long mod_select = 0x50544D00;   // | N, N < 256
constexpr unsigned long mod_select_mask = 0xFFFFFF00;
// Fatal renderer errors require reporting before an optional fresh-process restart.
constexpr unsigned long renderer_error = 0x50543547;
inline bool managed() {
    const char* value = std::getenv("GT5P_LAUNCHER_RESTART");
    return value && !std::strcmp(value, "1");
}
}
