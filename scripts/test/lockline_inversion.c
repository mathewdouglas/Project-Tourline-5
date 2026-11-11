/* The GT5 Phase 32 race freeze, reduced (lockline_inversion.py builds and
 * runs this against the runtime's real spu_coherency.c).
 *
 * Every thread is pinned to one CPU. An IDLE-priority holder takes the
 * lock-line lock, wakes the waiters and keeps working inside its critical
 * section; the ABOVE_NORMAL waiters preempt it and wait for the lock.
 * SwitchToThread never yields to a lower-priority thread, so with the old
 * spin the holder only runs again when Windows' starvation boost picks it,
 * seconds later. Prints how long the last waiter took to get the lock.
 * Exit 0; the Python driver judges the times. */
#include <windows.h>
#include <stdio.h>

void spu_lockline_lock(void);
void spu_lockline_unlock(void);

#define WAITERS 3
static HANDLE g_go;
static volatile LONG g_done;
static LARGE_INTEGER g_t0, g_freq;
static volatile double g_last_ms;

static DWORD WINAPI holder(LPVOID p)
{
    (void)p;
    spu_lockline_lock();
    QueryPerformanceCounter(&g_t0);
    SetEvent(g_go);                          /* the waiters preempt us here */
    volatile unsigned x = 0;                 /* ~2 ms of work inside the lock */
    LARGE_INTEGER a, b; QueryPerformanceCounter(&a);
    do { x++; QueryPerformanceCounter(&b); } while ((b.QuadPart - a.QuadPart) * 1000 < 2 * g_freq.QuadPart);
    spu_lockline_unlock();
    return 0;
}

static DWORD WINAPI waiter(LPVOID p)
{
    (void)p;
    WaitForSingleObject(g_go, INFINITE);
    spu_lockline_lock();
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    spu_lockline_unlock();
    const double ms = (double)(t.QuadPart - g_t0.QuadPart) * 1000.0 / (double)g_freq.QuadPart;
    if (ms > g_last_ms) g_last_ms = ms;
    InterlockedIncrement(&g_done);
    return 0;
}

int main(void)
{
    QueryPerformanceFrequency(&g_freq);
    g_go = CreateEventA(NULL, TRUE, FALSE, NULL);
    HANDLE t[WAITERS + 1];
    for (int i = 0; i <= WAITERS; i++) {
        t[i] = CreateThread(NULL, 0, i ? waiter : holder, NULL, CREATE_SUSPENDED, NULL);
        SetThreadAffinityMask(t[i], 1);
        SetThreadPriority(t[i], i ? THREAD_PRIORITY_ABOVE_NORMAL : THREAD_PRIORITY_IDLE);
    }
    for (int i = 1; i <= WAITERS; i++) ResumeThread(t[i]);
    Sleep(50);                               /* waiters parked on the event */
    ResumeThread(t[0]);
    const DWORD r = WaitForMultipleObjects(WAITERS + 1, t, TRUE, 20000);
    printf("waiters done %ld/%d; last lock after %.1f ms%s\n", g_done, WAITERS, g_last_ms,
           r == WAIT_TIMEOUT ? " (timed out at 20 s)" : "");
    return 0;
}
