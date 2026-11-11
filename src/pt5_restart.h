#pragma once
#include <cstdlib>
#include <cstring>
namespace pt5_restart {
constexpr unsigned long exit_code = 0x50543552;
inline bool managed() {
    const char* value = std::getenv("GT5P_LAUNCHER_RESTART");
    return value && !std::strcmp(value, "1");
}
}
