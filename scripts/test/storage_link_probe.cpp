// Drives launcher/storage_link.h for storage_link.py: storage_link_probe <home> <storage|->.
#include "../../launcher/storage_link.h"
#include <cstdio>
#include <cwchar>

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 2;
    const std::filesystem::path storage = wcscmp(argv[2], L"-") ? std::filesystem::path(argv[2]) : std::filesystem::path{};
    const auto problem = storage_link::sync(argv[1], storage);
    if (!problem.empty()) { fwprintf(stdout, L"%ls\n", problem.c_str()); return 1; }
    return 0;
}
