/* DPI awareness (issue #97).
 *
 * Without it Windows tells the game a 3840x2160 screen at 300% scaling is
 * 1280x720: the borderless window, the swap chain and the output pass were
 * all 1280x720 and Windows stretched the picture 3x. The process is now
 * per-monitor aware (v2), so the window and swap chain are real pixels.
 *
 * The F3 profiler and the shader badges are laid out in fixed pixels; their
 * threads are made DPI-unaware so Windows keeps scaling them as before
 * (mixed-mode DPI, Windows 10 1607 and later; a no-op before that).
 *
 * GT5P_DPI_AWARE=0: the old, DPI-unaware process. Both calls are looked up at
 * run time, so the exe still starts on systems without them. */
#pragma once
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

namespace gt5p_dpi {

inline bool enabled()
{
    const char* e = getenv("GT5P_DPI_AWARE");
    return !(e && e[0] == '0');
}

/* Process default, before any window is made. */
inline void make_process_aware()
{
    if (!enabled()) { fprintf(stderr, "[dpi] DPI-unaware (GT5P_DPI_AWARE=0)\n"); return; }
    HMODULE u = GetModuleHandleA("user32.dll");
    typedef BOOL (WINAPI *SetCtx)(HANDLE);
    typedef BOOL (WINAPI *SetAware)(void);
    SetCtx set_ctx = u ? (SetCtx)(void*)GetProcAddress(u, "SetProcessDpiAwarenessContext") : NULL;
    /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 is ((HANDLE)-4). */
    if (set_ctx && set_ctx((HANDLE)(INT_PTR)-4)) { fprintf(stderr, "[dpi] per-monitor DPI aware\n"); return; }
    SetAware set_aware = u ? (SetAware)(void*)GetProcAddress(u, "SetProcessDPIAware") : NULL;
    if (set_aware && set_aware()) fprintf(stderr, "[dpi] system DPI aware\n");
}

/* For a thread that makes fixed-pixel overlay windows. */
inline void make_thread_unaware()
{
    if (!enabled()) return;
    HMODULE u = GetModuleHandleA("user32.dll");
    typedef HANDLE (WINAPI *SetThreadCtx)(HANDLE);
    SetThreadCtx f = u ? (SetThreadCtx)(void*)GetProcAddress(u, "SetThreadDpiAwarenessContext") : NULL;
    if (f) f((HANDLE)(INT_PTR)-1);   /* DPI_AWARENESS_CONTEXT_UNAWARE */
}

}  // namespace gt5p_dpi
