/* The real sys_lwcond/sys_lwmutex handlers (runtime/ppu/ppu_sysprx.cpp,
 * force-included by sync_slots.py) with PS3_LWCOND_REAL=1, as GT5 runs.
 *
 * GT5 makes an lwmutex and an lwcond per task object, so a session uses
 * thousands of addresses. The runtime's slot tables kept every address for
 * good. Once the lwcond table (1024) was full, a wait on a new address
 * returned at once, and GT5's requester (which waits once:
 * `if (state != 3) wait`) freed a task its worker still used (issues #34,
 * #37, #43). Once the lwmutex table (65536) was full, a new lwmutex locked
 * nothing.
 *
 * lwcond: 3000 waits on distinct addresses, each woken by a signal, then a
 * wait on a fresh address that nobody signals. lwmutex: 70000 created, used
 * and destroyed, then a fresh one must exclude a second thread.
 * argv[1] = 1 expects the fix; 0 the old tables (both failures reproduced). */
#include <assert.h>
#include <atomic>
#include <thread>

static uint8_t g_mem[4u << 20];
extern "C" {
uint8_t* vm_base = g_mem;
uint32_t vm_read32(uint64_t a)
{ const uint8_t* p = g_mem + a; return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
uint64_t vm_read64(uint64_t a) { return (uint64_t)vm_read32(a) << 32 | vm_read32(a + 4); }
void vm_write32(uint64_t a, uint32_t v)
{ uint8_t* p = g_mem + a; p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
void vm_write64(uint64_t a, uint64_t v) { vm_write32(a, (uint32_t)(v >> 32)); vm_write32(a + 4, (uint32_t)v); }
uint8_t vm_read8(uint64_t a) { return g_mem[a]; }
void vm_write8(uint64_t a, uint8_t v) { g_mem[a] = v; }
/* The rest of what ppu_sysprx.cpp links against; none of it runs here. */
void ps3_hle_register_ctx(uint32_t, const char*, void (*)(ppu_context*)) {}
void ps3_gcm_pump_if_present(void) {}
void ppu_tls_set_template(uint32_t, uint32_t, uint32_t) {}
volatile long long g_gcm_ref_pub_count;
unsigned long long ppu_thread_cpu_us(unsigned) { return 0; }
unsigned ppu_thread_prof_pc(unsigned) { return 0; }
}

static const int32_t ETIMEDOUT_ = (int32_t)0x8001000Bu;
static const uint32_t MUTEX = 0x8000, ATTR = 0x9000, CONDS = 0x10000;
static const int N = 3000;

static int32_t call(void (*fn)(ppu_context*), uint32_t tid, uint64_t r3, uint64_t r4)
{
    ppu_context c; memset(&c, 0, sizeof c);
    c.thread_id = tid; c.gpr[3] = r3; c.gpr[4] = r4;
    fn(&c);
    return (int32_t)c.gpr[3];
}
static uint32_t cond(int i) { return CONDS + 16u * (uint32_t)i; }

int main(int argc, char** argv)
{
    const int fixed = argc > 1 && atoi(argv[1]) != 0;
    vm_write32(ATTR, 2);
    assert(call(sys_lwmutex_create, 1, MUTEX, ATTR) == 0);
    for (int i = 0; i <= N; i++) assert(call(sys_lwcond_create, 1, cond(i), MUTEX) == 0);

    /* A worker waits on each address in turn; the main thread signals it
     * until it is through (a signal with no waiter queued wakes nobody). */
    std::atomic<int> done{0};
    std::thread waiter([&] {
        for (int i = 0; i < N; i++) {
            assert(call(sys_lwmutex_lock, 7, MUTEX, 0) == 0);
            assert(call(sys_lwcond_wait, 7, cond(i), 0) == 0);
            assert(call(sys_lwmutex_unlock, 7, MUTEX, 0) == 0);
            done.store(i + 1);
        }
    });
    for (int i = 0; i < N; i++)
        while (done.load() <= i) { call(sys_lwcond_signal, 2, cond(i), 0); std::this_thread::yield(); }
    waiter.join();

    /* A fresh address nobody signals: lv2 blocks until the timeout. */
    assert(call(sys_lwmutex_lock, 7, MUTEX, 0) == 0);
    const ULONGLONG t0 = GetTickCount64();
    const int32_t r = call(sys_lwcond_wait, 7, cond(N), 300000);
    const ULONGLONG ms = GetTickCount64() - t0;
    assert(call(sys_lwmutex_unlock, 7, MUTEX, 0) == 0);
    printf("wait on a new address after %d others: ret=0x%08X after %llu ms\n", N, (uint32_t)r, ms);
    if (fixed) {
        assert(r == ETIMEDOUT_ && ms >= 250);
        /* A signal still reaches a waiter on a reused address. */
        std::thread w2([&] {
            assert(call(sys_lwmutex_lock, 7, MUTEX, 0) == 0);
            assert(call(sys_lwcond_wait, 7, cond(5), 2000000) == 0);
            assert(call(sys_lwmutex_unlock, 7, MUTEX, 0) == 0);
            done.store(-1);
        });
        while (done.load() != -1) { call(sys_lwcond_signal, 2, cond(5), 0); Sleep(1); }
        w2.join();
        puts("lwcond slots: freed slots are reused; waits keep blocking");
    } else {
        assert(r == 0 && ms < 100);
        puts("old lwcond table: the wait returned at once (reproduced)");
    }

    /* lwmutex: more distinct addresses than the table has slots. */
    const uint32_t MUTEXES = 0x100000, M = 70000;
    for (uint32_t i = 0; i < M; i++) {
        const uint32_t m = MUTEXES + 32u * i;
        assert(call(sys_lwmutex_create, 1, m, ATTR) == 0);
        assert(call(sys_lwmutex_lock, 1, m, 0) == 0);
        assert(call(sys_lwmutex_unlock, 1, m, 0) == 0);
        assert(call(sys_lwmutex_destroy_counted, 1, m, 0) == 0);
    }
    const uint32_t fresh = MUTEXES + 32u * M;
    assert(call(sys_lwmutex_create, 1, fresh, ATTR) == 0);
    assert(call(sys_lwmutex_lock, 1, fresh, 0) == 0);
    const int32_t t = call(sys_lwmutex_trylock, 2, fresh, 0);
    assert(call(sys_lwmutex_unlock, 1, fresh, 0) == 0);
    printf("trylock by another thread while held, after %u mutexes: 0x%08X\n", M, (uint32_t)t);
    if (fixed) {
        assert(t == ETIMEDOUT_);
        /* A destroyed address can be created again and still excludes. */
        const uint32_t again = MUTEXES + 32u * 7;
        assert(call(sys_lwmutex_create, 1, again, ATTR) == 0);
        assert(call(sys_lwmutex_lock, 1, again, 0) == 0);
        assert(call(sys_lwmutex_trylock, 2, again, 0) == ETIMEDOUT_);
        assert(call(sys_lwmutex_unlock, 1, again, 0) == 0);
        assert(call(sys_lwmutex_trylock, 2, again, 0) == 0);
        assert(call(sys_lwmutex_unlock, 2, again, 0) == 0);
        puts("lwmutex slots: destroyed slots are reused; new mutexes still exclude");
    } else {
        assert(t == 0);
        puts("old lwmutex table: a new mutex excluded nothing (reproduced)");
    }
    return 0;
}
