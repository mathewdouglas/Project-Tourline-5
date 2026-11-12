/* Native replacements hooked into lifted SPU job code (Phase 110).
 *
 * scripts/spu_native_patch.py puts a call to each function here at the top of
 * a lifted block ("spu_native_hooks" in config/targets.json). A replacement
 * only moves the guest state forward over work it can do natively and leaves
 * the block itself to finish, so every register still ends as the lifted
 * code leaves it.
 *
 * GT5P_SPU_NATIVE: 0 off; 1 on (default); 2 check: compute the result
 * natively, change nothing, and compare with what the lifted code then does.
 * GT5P_SPU_NATIVE_STATS=1 prints counts every 5 s (always on in check mode).
 */
#include "spu_context.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static volatile int s_mode = -1, s_stats = 0;

static int native_mode(void)
{
    int m = s_mode;
    if (m < 0) {
        const char* e = getenv("GT5P_SPU_NATIVE");
        m = e ? atoi(e) : 1;
        s_stats = m == 2 || getenv("GT5P_SPU_NATIVE_STATS") != NULL;
        s_mode = m;
        if (m) fprintf(stderr, "[spu-native] mode %d%s%c", m, m == 2 ? " (check)" : "", 10);
    }
    return m;
}

static volatile long long s_walks, s_skipped, s_ok, s_bad;

static void native_report(void)
{
    static volatile unsigned long long t0;
    struct timespec ts; timespec_get(&ts, TIME_UTC);
    unsigned long long now = (unsigned long long)ts.tv_sec * 1000u + (unsigned long long)ts.tv_nsec / 1000000u;
    unsigned long long was = t0;
    if (!was) { __atomic_compare_exchange_n(&t0, &was, now, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED); return; }
    if (now - was < 5000 || !__atomic_compare_exchange_n(&t0, &was, now, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        return;
    fprintf(stderr, "[spu-native] sorted walk: %lld walks, %lld nodes skipped natively; check %lld ok, %lld wrong%c",
            __atomic_exchange_n(&s_walks, 0, __ATOMIC_RELAXED), __atomic_exchange_n(&s_skipped, 0, __ATOMIC_RELAXED),
            __atomic_exchange_n(&s_ok, 0, __ATOMIC_RELAXED), __atomic_exchange_n(&s_bad, 0, __ATOMIC_RELAXED), 10);
}

/* A halfword or word of local store as rotqby takes it from the quadword
 * holding `a`: bytes past the quadword's end wrap to its start. */
static inline uint32_t ls_u16q(const uint8_t* ls, uint32_t a)
{
    uint32_t q = a & (SPU_LS_MASK & ~0xFu), o = a & 0xFu;
    if (__builtin_expect(o <= 14, 1)) { uint16_t v; memcpy(&v, ls + q + o, 2); return __builtin_bswap16(v); }
    return ((uint32_t)ls[q + o] << 8) | ls[q + ((o + 1) & 0xFu)];
}
static inline float ls_f32q(const uint8_t* ls, uint32_t a)
{
    uint32_t q = a & (SPU_LS_MASK & ~0xFu), o = a & 0xFu;
    if (__builtin_expect(o <= 12, 1)) {
        uint32_t v; memcpy(&v, ls + q + o, 4); v = __builtin_bswap32(v);
        float f; memcpy(&f, &v, 4); return f;
    }
    uint32_t w = ((uint32_t)ls[q + o] << 24) | ((uint32_t)ls[q + ((o + 1) & 0xFu)] << 16) |
                 ((uint32_t)ls[q + ((o + 2) & 0xFu)] << 8) | ls[q + ((o + 3) & 0xFu)];
    float f; memcpy(&f, &w, 4); return f;
}

/* Job body 0x0133A700, blocks 0x43E0/0x43E8/0x4420: inserting a node into a
 * list kept in descending order of a float key. r7 is the node the walk is
 * at; the next node's index is the halfword at +66 (0xFFFF ends the list);
 * nodes are 68 bytes from r13; the key is the float at +60; r12 holds the new
 * node's key. The walk moves on while the new key is not greater than the
 * next node's (fcgt), then 0x4420 links the new node in after r7. 40% of the
 * race's one-SPU workloads 5/6 ran this walk, one node per pass through the
 * lifted blocks.
 *
 * Natively: find the node the walk would stop at, and set r7 to the node one
 * step before it. The lifted code then takes that last step (which writes
 * every register an advancing pass writes) and the final test, so every
 * register ends as before. Nothing is written to local store. */
static _Thread_local int t_pending;
static _Thread_local uint32_t t_expect;

void gt5p_spu_sorted_walk(spu_context* ctx)
{
    const int mode = native_mode();
    if (!mode || (mode == 2 && t_pending)) return;
    const uint8_t* ls = ctx->ls;
    const uint32_t base = ctx->gpr[13]._u32[0];
    const float key = ctx->gpr[12]._f32[0];
    uint32_t p = ctx->gpr[7]._u32[0], back1 = p;
    unsigned n = 0;
    for (; n < 0x10000u; n++) {
        uint32_t nx = ls_u16q(ls, p + 66);
        if (nx == 0xFFFFu) break;
        uint32_t q = base + (uint32_t)((int32_t)(int16_t)nx * 68);
        if (key > ls_f32q(ls, q + 60)) break;
        back1 = p; p = q;
    }
    if (mode == 2) {
        t_pending = 1; t_expect = p;
        __atomic_fetch_add(&s_walks, 1, __ATOMIC_RELAXED);
        native_report();
        return;
    }
    if (n >= 2) ctx->gpr[7]._u32[0] = back1;   /* the lifted code takes the last step */
    if (s_stats) {
        __atomic_fetch_add(&s_walks, 1, __ATOMIC_RELAXED);
        if (n >= 2) __atomic_fetch_add(&s_skipped, n - 1, __ATOMIC_RELAXED);
        native_report();
    }
}

/* 0x4420, where the walk ends: in check mode, the node it actually stopped at
 * against the native answer. */
void gt5p_spu_sorted_walk_end(spu_context* ctx)
{
    if (s_mode != 2 || !t_pending) return;
    t_pending = 0;
    if (ctx->gpr[7]._u32[0] == t_expect) __atomic_fetch_add(&s_ok, 1, __ATOMIC_RELAXED);
    else {
        long long b = __atomic_fetch_add(&s_bad, 1, __ATOMIC_RELAXED);
        if (b < 8)
            fprintf(stderr, "[spu-native] sorted walk MISMATCH: lifted stopped at 0x%05X, native 0x%05X%c",
                    ctx->gpr[7]._u32[0], t_expect, 10);
    }
}

/* ---------------------------------------------------------------------------
 * Gran Turismo 5 (Phase 31): a loop in workload 4's job body 0x0196FF40.
 */

#ifdef _WIN32
#include <windows.h>
#include <intrin.h>
#endif
static volatile long long s_clears, s_clear_words;
static void native_report2(void)
{
    if (!s_stats) return;
    static volatile long long t0;
    long long now = (long long)GetTickCount64(), was = t0;
    if (!was) { InterlockedCompareExchange64(&t0, now, 0); return; }
    if (now - was < 5000 || InterlockedCompareExchange64(&t0, now, was) != was) return;
    fprintf(stderr, "[spu-native] GT5 table clears %lld (%lld words)%c",
            InterlockedExchange64(&s_clears, 0), InterlockedExchange64(&s_clear_words, 0), 10);
}
/* LS 0x4C14: zeroes words r15, r15+4, ... up to r6 (1280) at local store
 * r83 + r15 (lqx, cwx, shufb of r7 = 0, stqx; ~3% of the lane time). Here the
 * words up to the last are zeroed natively and r15 is moved to the last one,
 * so the lifted loop's final iteration leaves r15, r69-r72 as before. Only
 * when r7 is zero and the counter is word aligned below the end. */
void gt5p_spu_clear_words(spu_context* ctx)
{
    if (!native_mode()) return;
    const uint32_t r15 = ctx->gpr[15]._u32[0], end = ctx->gpr[6]._u32[0], base = ctx->gpr[83]._u32[0];
    const u128* r7 = &ctx->gpr[7];
    if ((r15 & 3) || (end & 3) || end <= r15 || end - r15 <= 4 || end - r15 > SPU_LS_SIZE) return;
    if (r7->_u32[0] | r7->_u32[1] | r7->_u32[2] | r7->_u32[3]) return;
    const int check = native_mode() == 2;
    if (!check) {
        /* The word stores are contiguous, including a possible LS wrap.
         * Keep the last iteration lifted so all scratch registers agree. */
        const uint32_t start = (r15 + base) & SPU_LS_MASK & ~3u;
        const uint32_t bytes = end - 4 - r15;
        const uint32_t first = bytes < SPU_LS_SIZE - start ? bytes : SPU_LS_SIZE - start;
        memset(ctx->ls + start, 0, first);
        if (bytes > first) memset(ctx->ls, 0, bytes - first);
    }
    if (!check) for (int i = 0; i < 4; i++) ctx->gpr[15]._u32[i] += (end - 4) - r15;
    if (s_stats) { InterlockedIncrement64(&s_clears); InterlockedAdd64(&s_clear_words, (end - 4 - r15) / 4); native_report2(); }
}

/* Register promotion of hot lifted functions (GT5 Phase 44).
 *
 * scripts/spu_regpromo.py gives each function listed under "spu_regpromo" in
 * config/targets.json a copy that keeps the SPU registers it uses in C locals
 * (<fn>_rp), and makes the function's first statement a switch on this mode:
 * GT5P_SPU_REGPROMO=0 the lifted code as before (default until measured);
 * 1 the promoted copy; 2 check: every GT5P_SPU_REGPROMO_CHECK_EVERY-th entry
 * (default 64) runs the original and the promoted body from the same state as
 * far as their first exit, compares registers, pc, interrupt enable and the
 * whole local store, puts the state back and lets the original run. Counts
 * every 5 s; the first 16 mismatches are printed in full. 3: A/B within one
 * boot, the promoted copy while g_spu_exp is set (SPU_EXP_AB=1 with
 * SPURS_LANE_CYC=1 flips it every 5 s; scripts/diag/abcyc.py --key exp). */
volatile int g_gt5p_spu_regpromo = -1;
int gt5p_spu_regpromo_init(void)
{
    const char* e = getenv("GT5P_SPU_REGPROMO");
    int m = e ? atoi(e) : 0;
    if (m < 0 || m > 3) m = 0;
    if (m && g_gt5p_spu_regpromo < 0)
        fprintf(stderr, "[spu-regpromo] mode %d%s%c", m, m == 2 ? " (check)" : m == 3 ? " (A/B on g_spu_exp)" : "", 10);
    g_gt5p_spu_regpromo = m;
    return m;
}

static SPU_THREAD_LOCAL uint8_t* t_rp_ls0;
static SPU_THREAD_LOCAL uint8_t* t_rp_lsA;
static SPU_THREAD_LOCAL unsigned t_rp_n;
static volatile long long s_rp_checks, s_rp_bad;
static volatile long s_rp_printed;

static void rp_report(void)
{
    static volatile long long t0;
    long long now = (long long)GetTickCount64(), was = t0;
    if (!was) { InterlockedCompareExchange64(&t0, now, 0); return; }
    if (now - was < 5000 || InterlockedCompareExchange64(&t0, now, was) != was) return;
    fprintf(stderr, "[spu-regpromo] check: %lld compared, %lld differ%c",
            InterlockedExchange64(&s_rp_checks, 0), InterlockedExchange64(&s_rp_bad, 0), 10);
}

void gt5p_spu_regpromo_check(spu_context* ctx, void (*orig)(spu_context*),
                             void (*promo)(spu_context*), uint32_t block)
{
    static int every = 0;
    if (!every) { const char* e = getenv("GT5P_SPU_REGPROMO_CHECK_EVERY"); every = e && atoi(e) > 0 ? atoi(e) : 64; }
    if ((t_rp_n++ % (unsigned)every) != 0) return;
    if (!t_rp_ls0) {
        t_rp_ls0 = (uint8_t*)malloc(SPU_LS_SIZE);
        t_rp_lsA = (uint8_t*)malloc(SPU_LS_SIZE);
        if (!t_rp_ls0 || !t_rp_lsA) { free(t_rp_ls0); free(t_rp_lsA); t_rp_ls0 = t_rp_lsA = NULL; return; }
    }
    static SPU_THREAD_LOCAL u128 g0[128], gA[128];
    const uint32_t pc0 = ctx->pc, ie0 = ctx->int_enable;
    memcpy(g0, ctx->gpr, sizeof g0);
    memcpy(t_rp_ls0, ctx->ls, SPU_LS_SIZE);
    orig(ctx);
    const uint32_t pcA = ctx->pc, ieA = ctx->int_enable;
    memcpy(gA, ctx->gpr, sizeof gA);
    memcpy(t_rp_lsA, ctx->ls, SPU_LS_SIZE);
    memcpy(ctx->gpr, g0, sizeof g0); ctx->pc = pc0; ctx->int_enable = ie0;
    memcpy(ctx->ls, t_rp_ls0, SPU_LS_SIZE);
    promo(ctx);
    int bad_reg = -1;
    for (int r = 0; r < 128 && bad_reg < 0; r++)
        if (memcmp(&gA[r], &ctx->gpr[r], sizeof gA[r])) bad_reg = r;
    int bad_ls = -1;
    if (memcmp(t_rp_lsA, ctx->ls, SPU_LS_SIZE))
        for (int a = 0; a < SPU_LS_SIZE; a += 16)
            if (memcmp(t_rp_lsA + a, ctx->ls + a, 16)) { bad_ls = a; break; }
    const int bad = bad_reg >= 0 || bad_ls >= 0 || pcA != ctx->pc || ieA != ctx->int_enable;
    if (bad && InterlockedIncrement(&s_rp_printed) <= 16) {
        fprintf(stderr, "[spu-regpromo] MISMATCH block 0x%X img=%d: pc %05X/%05X ie %u/%u",
                block, ctx->image_id, pcA, ctx->pc, ieA, ctx->int_enable);
        if (bad_reg >= 0)
            fprintf(stderr, " r%d %08X%08X%08X%08X/%08X%08X%08X%08X", bad_reg,
                    gA[bad_reg]._u32[0], gA[bad_reg]._u32[1], gA[bad_reg]._u32[2], gA[bad_reg]._u32[3],
                    ctx->gpr[bad_reg]._u32[0], ctx->gpr[bad_reg]._u32[1],
                    ctx->gpr[bad_reg]._u32[2], ctx->gpr[bad_reg]._u32[3]);
        if (bad_ls >= 0) fprintf(stderr, " ls 0x%05X", bad_ls);
        fprintf(stderr, "%c", 10);
    }
    memcpy(ctx->gpr, g0, sizeof g0); ctx->pc = pc0; ctx->int_enable = ie0;
    memcpy(ctx->ls, t_rp_ls0, SPU_LS_SIZE);
    InterlockedIncrement64(&s_rp_checks);
    if (bad) InterlockedIncrement64(&s_rp_bad);
    rp_report();
}

/* GT5P_SPU_CAPTURE=<file>: with scripts/diag/spu_capture_patch.py, append
 * the SPU state (registers, local store, pc) at entry of each patched lifted
 * function to <file>, for scripts/test/spu_loop_bench.py. Each function
 * (up to 16) keeps its own schedule: its first state is taken
 * GT5P_SPU_CAPTURE_AFTER_MS after its first call (default 20000), then one
 * every GT5P_SPU_CAPTURE_GAP_MS (default 500), GT5P_SPU_CAPTURE_N per
 * function (default 8). The file holds game-derived data: keep it under
 * scratch/, never track it. Record: "SPUCAP1\0", u32 function address, u32
 * pc, u32 image id, u32 calls of that function so far, then gpr[128] and
 * the 256 KB local store as in spu_context. */
void gt5p_spu_capture(spu_context* ctx, uint32_t addr)
{
    static volatile LONG init;
    static LONG n = 8, open_slots = 0, done_slots = 0;
    static ULONGLONG after = 20000, gap = 500;
    static FILE* f;
    static SRWLOCK lk = SRWLOCK_INIT;
    static struct { volatile uint32_t addr; volatile LONG calls, written; ULONGLONG next; } slot[16];
    if (init == 0) {
        AcquireSRWLockExclusive(&lk);
        if (init == 0) {
            const char* e = getenv("GT5P_SPU_CAPTURE");
            if (e && *e) f = fopen(e, "wb");
            if ((e = getenv("GT5P_SPU_CAPTURE_AFTER_MS"))) after = (ULONGLONG)atol(e);
            if ((e = getenv("GT5P_SPU_CAPTURE_GAP_MS")) && atol(e) > 0) gap = (ULONGLONG)atol(e);
            if ((e = getenv("GT5P_SPU_CAPTURE_N"))) n = atol(e);
            init = f ? 1 : -1;
        }
        ReleaseSRWLockExclusive(&lk);
    }
    if (init < 0) return;
    int k = 0;
    while (k < 16 && slot[k].addr && slot[k].addr != addr) k++;
    if (k == 16) return;
    if (!slot[k].addr) {
        AcquireSRWLockExclusive(&lk);
        k = 0;
        while (k < 16 && slot[k].addr && slot[k].addr != addr) k++;
        if (k < 16 && !slot[k].addr) { slot[k].next = GetTickCount64() + after; slot[k].addr = addr; open_slots++; }
        ReleaseSRWLockExclusive(&lk);
        if (k == 16) return;
    }
    const LONG c = InterlockedIncrement(&slot[k].calls);
    if (slot[k].written >= n || GetTickCount64() < slot[k].next) return;
    AcquireSRWLockExclusive(&lk);
    if (f && slot[k].written < n && GetTickCount64() >= slot[k].next) {
        slot[k].next = GetTickCount64() + gap;
        const uint32_t hdr[4] = { addr, ctx->pc, (uint32_t)ctx->image_id, (uint32_t)c };
        fwrite("SPUCAP1", 1, 8, f);
        fwrite(hdr, sizeof hdr, 1, f);
        fwrite(ctx->gpr, sizeof ctx->gpr, 1, f);
        fwrite(ctx->ls, SPU_LS_SIZE, 1, f);
        fflush(f);
        if (++slot[k].written == n) {
            fprintf(stderr, "[spu-capture] %ld states of 0x%X written (%ld calls)%c", (long)n, addr, (long)c, 10);
            if (++done_slots == open_slots) { fclose(f); f = NULL; }
        }
    }
    ReleaseSRWLockExclusive(&lk);
}

/* GT5P_SPU_REGION (scripts/spu_region.py, GT5 Phase 45): 1 (default) the
 * merged copies of the listed lifted loops, with the registers in locals;
 * 0 the lifted functions as they are; 3 the merged copy while g_spu_exp is
 * set (SPU_EXP_AB=1 SPURS_LANE_CYC=1 flips it: A/B within one boot).
 * Measured at Eiger Nordwand: frames 7.5-8% shorter with it, within one
 * boot; the loops are identical in registers and local store
 * (scripts/test/spu_loop_bench.py). Only targets that list spu_regions have
 * merged copies. */
volatile int g_gt5p_spu_region = -1;
int gt5p_spu_region_init(void)
{
    const char* e = getenv("GT5P_SPU_REGION");
    int m = e ? atoi(e) : 1;
    if (m != 1 && m != 3) m = 0;
    if (m != 1 && g_gt5p_spu_region < 0)
        fprintf(stderr, "[spu-region] mode %d%s%c", m, m == 3 ? " (A/B on g_spu_exp)" : "", 10);
    g_gt5p_spu_region = m;
    return m;
}
