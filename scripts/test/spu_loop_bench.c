/* Driver for scripts/test/spu_loop_bench.py: replay captured SPU states
 * (scripts/diag/spu_capture_patch.py, GT5P_SPU_CAPTURE) through one lifted
 * function of one job body, built standalone with this build's helpers.
 * Prints, per state, the median time of one call and a hash of the final
 * registers and local store, so builds with different helpers can be timed
 * and compared exactly.
 *   bench.exe <states file> <repeats> [entry:lo:hi,...] */
#include "spu_helpers.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

volatile int g_spu_fast = 1;
volatile int g_spu_chain_back = 1;
unsigned g_spu_stack_reset_n;
int g_spu_ls_watch_n;
SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*);

#define MAXF 8192
static struct { uint32_t addr; void (*fn)(spu_context*); } s_fn[MAXF];
static int s_nfn;
static void bench_register(uint32_t addr, void (*fn)(spu_context*)) { if (s_nfn < MAXF) { s_fn[s_nfn].addr = addr; s_fn[s_nfn].fn = fn; s_nfn++; } }
static void (*lookup(uint32_t pc))(spu_context*)
{
    for (int i = 0; i < s_nfn; i++) if (s_fn[i].addr == (pc & 0x3FFFCu)) return s_fn[i].fn;
    return NULL;
}
static int s_bench_bad, s_bench_left;
static uint32_t s_lo, s_hi = 0x40000;   /* the routine's address range (argv[3]) */
static uint32_t s_ret;                   /* the call's return address (r0 on entry) */
/* The runtime's indirect branch: here a lookup in this image's table. A
 * target outside the routine's range ends the run there (s_bench_left): routines
 * that leave through a register branch instead of returning. */
void spu_indirect_branch(spu_context* ctx)
{
    const uint32_t pc = ctx->pc & 0x3FFFCu;
    if (pc < s_lo || pc >= s_hi || pc == (s_ret & 0x3FFFCu)) { s_bench_left = 1; return; }
    void (*f)(spu_context*) = lookup(pc);
    if (!f) { s_bench_bad = 1; return; }
    f(ctx);
}
/* Runtime entry points the measured path must never reach. */
int g_spu_drain_ret_fast = 1;
static void unreached(const char* what) { fprintf(stderr, "%s reached: not a self-contained path\n", what); exit(3); }
void spu_drain_call_impl(spu_context* ctx, uint32_t return_pc) { (void)ctx; (void)return_pc; unreached("spu_drain_call_impl"); }
u128 spu_rdch(spu_context* ctx, uint32_t channel) { (void)ctx; (void)channel; unreached("spu_rdch"); return (u128){0}; }
void spu_wrch(spu_context* ctx, uint32_t channel, u128 value) { (void)ctx; (void)channel; (void)value; unreached("spu_wrch"); }
void spu_halt(spu_context* ctx) { (void)ctx; unreached("spu_halt"); }
void spu_stop(spu_context* ctx) { (void)ctx; unreached("spu_stop"); }
/* The native replacements and the switches of the post-lift steps already
 * in generated/ (spu_native_patch.py, spu_regpromo.py, spu_region.py), as
 * the game builds them. main() sets the regions and register promotion off
 * unless this variant is built to test them. */
volatile int g_spu_exp;
#include "../../src/gt5p_spu_native.c"
#ifdef BENCH_COUNT
static unsigned s_cnt[0x40000 / 4];
void bench_count(unsigned a) { s_cnt[(a & 0x3FFFF) / 4]++; }
#endif
#define spu_register_function bench_register
#include BENCH_LIFTED
#undef spu_register_function

typedef struct { char magic[8]; uint32_t addr, pc, image, calls; } cap_hdr;
static spu_context s_ctx, s_init;

static uint64_t fnv(const void* p, size_t n, uint64_t h)
{
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ULL; }
    return h;
}

int main(int argc, char** argv)
{
    if (argc < 3) { fprintf(stderr, "usage: bench <states> <repeats>\n"); return 2; }
#ifdef BENCH_REGION
    _putenv("GT5P_SPU_REGION=1");
#else
    _putenv("GT5P_SPU_REGION=0");
#endif
#if defined(BENCH_RP) && (!defined(BENCH_RP_MODE) || BENCH_RP_MODE)
    _putenv("GT5P_SPU_REGPROMO=1");
#else
    _putenv("GT5P_SPU_REGPROMO=0");
#endif
    BENCH_REGISTER();
    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    const int reps = atoi(argv[2]);
    SetThreadAffinityMask(GetCurrentThread(), 1u << 2);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    LARGE_INTEGER qf; QueryPerformanceFrequency(&qf);
    cap_hdr h; int k = 0; double total = 0;
    while (fread(&h, sizeof h, 1, f) == 1) {
        if (memcmp(h.magic, "SPUCAP1", 8)) { fprintf(stderr, "bad record\n"); return 2; }
        memset(&s_init, 0, sizeof s_init);
        if (fread(s_init.gpr, sizeof s_init.gpr, 1, f) != 1 || fread(s_init.ls_store, SPU_LS_SIZE, 1, f) != 1) return 2;
        void (*entry)(spu_context*) = lookup(h.addr);
        if (!entry) { fprintf(stderr, "no function 0x%X\n", h.addr); return 2; }
        const uint32_t ret = s_init.gpr[0]._u32[0];
        s_ret = ret;
        /* argv[3]: "entry:lo:hi,..." (hex), the address range of each
         * captured routine; default: the whole local store. */
        s_lo = 0; s_hi = 0x40000;
        for (const char* q = argc > 3 ? argv[3] : ""; *q; ) {
            char* e; unsigned long a = strtoul(q, &e, 16), lo = 0, hi = 0x40000;
            if (*e == ':') lo = strtoul(e + 1, &e, 16);
            if (*e == ':') hi = strtoul(e + 1, &e, 16);
            if (a == h.addr) { s_lo = (uint32_t)lo; s_hi = (uint32_t)hi; }
            q = (*e == ',') ? e + 1 : e;
            if (*e != ',') break;
        }
        double best[64]; int nb = 0; uint64_t hash = 0;
        for (int r = 0; r < reps; r++) {
            memcpy(&s_ctx, &s_init, sizeof s_ctx);
            s_ctx.ls = s_ctx.ls_store;
            s_ctx.pc = h.addr; s_ctx.image_id = (int)h.image; s_ctx.host_depth = 1; s_ctx.int_enable = 0;
            g_spu_trampoline_fn = NULL; s_bench_bad = 0; s_bench_left = 0;
            LARGE_INTEGER t0, t1; QueryPerformanceCounter(&t0);
            entry(&s_ctx);
            for (int guard = 0; g_spu_trampoline_fn && !s_bench_left && guard < 100000000; guard++) {
                void (*t)(spu_context*) = g_spu_trampoline_fn; g_spu_trampoline_fn = NULL; t(&s_ctx);
            }
            QueryPerformanceCounter(&t1);
            if (s_bench_bad
#ifndef BENCH_BLOCK
                || (!s_bench_left && s_ctx.pc != ret)
#endif
                ) { fprintf(stderr, "state %d: ended at pc 0x%X, expected 0x%X (bad=%d)\n", k, s_ctx.pc, ret, s_bench_bad); return 1; }
            const double us = (double)(t1.QuadPart - t0.QuadPart) * 1e6 / (double)qf.QuadPart;
            if (nb < 64) best[nb++] = us;
            const uint32_t endpc = s_ctx.pc;
            uint64_t hh = fnv(&endpc, 4, fnv(s_ctx.ls_store, SPU_LS_SIZE, fnv(s_ctx.gpr, sizeof s_ctx.gpr, 1469598103934665603ULL)));
            hh=fnv(&s_ctx.int_enable,sizeof s_ctx.int_enable,hh);
            if (r && hh != hash) { fprintf(stderr, "state %d: not deterministic\n", k); return 1; }
            hash = hh;
        }
        for (int i = 1; i < nb; i++) for (int j = i; j > 0 && best[j] < best[j - 1]; j--) { double t = best[j]; best[j] = best[j - 1]; best[j - 1] = t; }
        printf("state %2d entry %05X calls %6u: %9.1f us  hash %016llX\n", k, h.addr, h.calls, best[nb / 2], (unsigned long long)hash);
        total += best[nb / 2]; k++;
    }
    printf("total %.1f us over %d states\n", total, k);
#ifdef BENCH_COUNT
    printf("counts (function entries per call, all states):\n");
    for (int n = 0; n < 25; n++) {
        unsigned bi = 0;
        for (unsigned i = 0; i < 0x10000; i++) if (s_cnt[i] > s_cnt[bi]) bi = i;
        if (!s_cnt[bi]) break;
        printf("  0x%05X %10.1f\n", bi * 4, (double)s_cnt[bi] / reps / k);
        s_cnt[bi] = 0;
    }
#endif
    return 0;
}
