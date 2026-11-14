#pragma once
#include <cstdlib>
#include <cstring>
namespace pt5_restart {
constexpr unsigned long exit_code = 0x50543552;
constexpr unsigned long version_104 = 0x50543104;
constexpr unsigned long version_211 = 0x50543211;
constexpr unsigned long setup_211 = 0x50543521;
inline bool managed() {
    const char* value = std::getenv("GT5P_LAUNCHER_RESTART");
    return value && !std::strcmp(value, "1");
}
}
