/* The real sys_lwmutex handlers (runtime/ppu/ppu_sysprx.cpp, force-included
 * by lwmutex_host_state.py), with a guest that leaves a stale recursion count
 * in a freed lwmutex: issue #37, a GT5 2.11 loader worker finishing a request
 * whose owner had already returned and reused that stack frame.
 *
 * argv[1] = 1 expects the fix (PS3_LWM_HOST_STATE on); 0 reproduces the old
 * hang, seen here as the second lock timing out. */
#include <assert.h>

static uint8_t g_mem[1u << 20];
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
/* The rest of what ppu_sysprx.cpp links against; none of it runs here.
 * Anything else stays unresolved (/FORCE:UNRESOLVED) and is never called. */
void ps3_hle_register_ctx(uint32_t, const char*, void (*)(ppu_context*)) {}
void ps3_gcm_pump_if_present(void) {}
void ppu_tls_set_template(uint32_t, uint32_t, uint32_t) {}
volatile long long g_gcm_ref_pub_count;
unsigned long long ppu_thread_cpu_us(unsigned) { return 0; }
unsigned ppu_thread_prof_pc(unsigned) { return 0; }
}

static const uint32_t A = 0x8000;        /* the request's lwmutex */
static const int32_t ETIMEDOUT_ = (int32_t)0x8001000Bu;

static int32_t call(void (*fn)(ppu_context*), uint32_t tid, uint64_t r3, uint64_t r4)
{
    ppu_context c; memset(&c, 0, sizeof c);
    c.thread_id = tid; c.gpr[3] = r3; c.gpr[4] = r4;
    fn(&c);
    return (int32_t)c.gpr[3];
}

int main(int argc, char** argv)
{
    const int fixed = argc > 1 && atoi(argv[1]) != 0;
    const uint32_t attr = 0x9000;
    vm_write32(attr, 2);                                  /* protocol */
    assert(call(sys_lwmutex_create, 42, A, attr) == 0);   /* requester builds it */

    /* Ordinary recursion still works: lock twice, unlock twice, then free. */
    assert(call(sys_lwmutex_lock, 9, A, 0) == 0);
    assert(call(sys_lwmutex_lock, 9, A, 0) == 0);
    assert(vm_read32(A + 0xC) == 2);                      /* guest copy kept */
    assert(call(sys_lwmutex_trylock, 10, A, 0) == ETIMEDOUT_);
    assert(call(sys_lwmutex_unlock, 9, A, 0) == 0);
    assert(call(sys_lwmutex_trylock, 10, A, 0) == ETIMEDOUT_);
    assert(call(sys_lwmutex_unlock, 9, A, 0) == 0);
    assert(call(sys_lwmutex_trylock, 10, A, 0) == 0);
    assert(call(sys_lwmutex_unlock, 10, A, 0) == 0);

    /* A condition wait releases a recursive hold and restores it (the poll
     * form: PS3_LWCOND_REAL is unset here). */
    const uint32_t C = 0x8100;
    assert(call(sys_lwcond_create, 9, C, A) == 0);
    assert(call(sys_lwmutex_lock, 9, A, 0) == 0);
    assert(call(sys_lwmutex_lock, 9, A, 0) == 0);
    assert(call(sys_lwcond_wait, 9, C, 0) == 0);
    assert(vm_read32(A + 0x0) == 9 && vm_read32(A + 0xC) == 2);
    assert(call(sys_lwmutex_unlock, 9, A, 0) == 0);
    assert(call(sys_lwmutex_trylock, 10, A, 0) == ETIMEDOUT_);
    assert(call(sys_lwmutex_unlock, 9, A, 0) == 0);
    assert(call(sys_lwmutex_trylock, 10, A, 0) == 0);
    assert(call(sys_lwmutex_unlock, 10, A, 0) == 0);

    /* The worker holds the request's lock (0x00A6F4B4). The requester has
     * returned; its next calls reuse the frame and leave stack data in the
     * lwmutex words. */
    assert(call(sys_lwmutex_lock, 9, A, 0) == 0);
    vm_write32(A + 0x0, 0x30C2373Cu);
    vm_write32(A + 0xC, 0x00007C36u);
    assert(call(sys_lwmutex_unlock, 9, A, 0) == 0);

    /* The worker locks it again to clear the busy byte (0x00A6BDF4). In the
     * game the timeout is infinite; 200 ms stands in for "never". */
    const int32_t r = call(sys_lwmutex_lock, 9, A, 200000);
    if (fixed) {
        assert(r == 0);
        assert(call(sys_lwmutex_unlock, 9, A, 0) == 0);
        assert(call(sys_lwmutex_trylock, 10, A, 0) == 0);
        assert(call(sys_lwmutex_unlock, 10, A, 0) == 0);
        puts("lwmutex host state: stale guest words no longer hold the lock");
    } else {
        assert(r == ETIMEDOUT_);
        puts("legacy guest-word state: worker's second lock never returns (reproduced)");
    }
    return 0;
}
