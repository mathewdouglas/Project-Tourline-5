/* Game script errors in the log (scripts/script_error_patch.py).
 *
 * GT5's script engine reports an error through one routine (2.11
 * 0x009C3048, 01.04 0x00B3FE1C; `script_error_func` in config/targets.json):
 * r3 the kind ("FatalError"), r4 the source file ("m_adhoc.cpp"), r5 the
 * line, r6 a context string, r7 a std::string whose first word points at
 * the formatted message. In its throwing mode the routine then throws a C++
 * exception; the port cannot deliver guest exceptions to their handlers, so
 * the game terminates with exit code 1 (#55/#56/#57). This prints the
 * message first, so a log and a crash report say which error it was:
 *
 *   [script-error] FatalError m_adhoc.cpp:864: nil object cannot be used as ...
 *
 * The routine then composes the full text (2.11 0x009C2E60: the message plus
 * the script engine's frames, `name:line;` innermost first) into a
 * std::string; `script_error_compose_ret` in config/targets.json is where
 * that call returns, and the patch prints the composed text there too:
 *
 *   [script-error]   <message> name:line;name:line;...
 *
 * At most 64 lines a run each. GT5P_SCRIPT_ERROR_LOG=0: nothing is printed. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>

extern "C" {
extern uint8_t* vm_base;
uint8_t vm_read8(uint64_t);
uint32_t vm_read32(uint64_t);
}

namespace {

bool readable(uint32_t a, uint32_t bytes)
{
    if (!vm_base || a < 0x10000 || a >= 0xF0000000u || bytes > 0xF0000000u - a) return false;
    uintptr_t p = (uintptr_t)vm_base + a, end = p + bytes;
    while (p < end) {
        MEMORY_BASIC_INFORMATION m{};
        if (!VirtualQuery((void*)p, &m, sizeof m) || m.State != MEM_COMMIT ||
            (m.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
        const uintptr_t next = (uintptr_t)m.BaseAddress + m.RegionSize;
        if (next <= p) return false;
        p = next;
    }
    return true;
}

/* A guest C string, printable bytes only, at most `cap` characters. */
std::string guest_text(uint32_t a, size_t cap)
{
    std::string s;
    if (!a) return "(null)";
    for (size_t i = 0; i < cap; i++) {
        if ((i & 0xFFF) == 0 || ((a + i) & 0xFFF) == 0)
            if (!readable(a + (uint32_t)i, 1)) { s += "(unreadable)"; break; }
        const char c = (char)vm_read8(a + i);
        if (!c) return s;
        s += (c >= 0x20 && c < 0x7F) ? c : '?';
    }
    return s + "...";
}

} // namespace

extern "C" void gt5p_script_error(uint64_t kind, uint64_t file, uint64_t line, uint64_t context, uint64_t message)
{
    static int on = -1;
    static volatile long printed = 0;
    if (on < 0) { const char* e = getenv("GT5P_SCRIPT_ERROR_LOG"); on = !(e && e[0] == '0'); }
    if (!on || InterlockedIncrement(&printed) > 64) return;
    const uint32_t m = (uint32_t)message;
    const uint32_t text = readable(m, 4) ? vm_read32(m) : 0;
    std::string ctx = context ? guest_text((uint32_t)context, 120) : std::string();
    fprintf(stderr, "[script-error] %s %s:%d%s%s: %s\n",
            guest_text((uint32_t)kind, 40).c_str(), guest_text((uint32_t)file, 80).c_str(), (int)(int32_t)line,
            ctx.empty() ? "" : " ", ctx.c_str(), guest_text(text, 400).c_str());
    fflush(stderr);
}

/* str: the guest std::string object (its first word points at the characters;
 * the length is 12 bytes before them). */
extern "C" void gt5p_script_error_text(uint64_t str)
{
    static int on = -1;
    static volatile long printed = 0;
    if (on < 0) { const char* e = getenv("GT5P_SCRIPT_ERROR_LOG"); on = !(e && e[0] == '0'); }
    if (!on || !readable((uint32_t)str, 4)) return;
    const uint32_t chars = vm_read32((uint32_t)str);
    if (!readable(chars - 12, 12) || InterlockedIncrement(&printed) > 64) return;
    const uint32_t len = vm_read32(chars - 12);
    fprintf(stderr, "[script-error]   %s%c", guest_text(chars, len < 2000 ? len : 2000).c_str(), 10);
    fflush(stderr);
}
