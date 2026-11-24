#include "spu_helpers.h"
#include <assert.h>
#include "../../src/gt5p_spu_native.c"
#include "fill.inc"

int g_spu_ls_watch_n;
static uint32_t seed = 0x20261122;
static uint32_t rnd(void) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; }
static spu_context ref, opt;
static __attribute__((noinline)) void original(spu_context* ctx) { lifted_fill(ctx); }
static __attribute__((noinline)) void native(spu_context* ctx) { gt5p_spu_fill_lines(ctx); lifted_fill(ctx); }
static double bench(void (*volatile fn)(spu_context*), unsigned bytes, int zero)
{
    memset(&opt, 0, sizeof opt); opt.ls = opt.ls_store;
    opt.image_id = 1; opt.policy_mode = 1; opt.gpr[3]._u32[0] = 1024 + bytes;
    if (!zero) for (unsigned k = 0; k < 4; k++) opt.gpr[5]._u32[k] = rnd();
    LARGE_INTEGER frequency, start, end; QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    for (unsigned r = 0; r < 1000; r++) { opt.gpr[4]._u32[0] = 1024; fn(&opt); }
    QueryPerformanceCounter(&end);
    assert(opt.gpr[4]._u32[0] == 1024 + bytes);
    return 1e3 * (double)(end.QuadPart - start.QuadPart) / (double)frequency.QuadPart;
}

static void unchanged(void)
{
    ref = opt;
    gt5p_spu_fill_lines(&opt);
    assert(!memcmp(&ref, &opt, sizeof opt));
}

int main(void)
{
    s_mode = 1; s_stats = 0; s_fill_mode = 1; g_spu_smc_watch = 0;
    for (unsigned t = 0; t < 2400; t++) {
        memset(&ref, 0, sizeof ref); ref.ls = ref.ls_store;
        ref.image_id = 1; ref.policy_mode = 1; ref.int_enable = t & 1;
        for (unsigned i = 0; i < SPU_LS_SIZE; i += 4) { uint32_t v = rnd(); memcpy(ref.ls + i, &v, 4); }
        for (unsigned r = 0; r < 128; r++) for (unsigned k = 0; k < 4; k++) ref.gpr[r]._u32[k] = rnd();
        unsigned from = rnd(), bytes = (1 + rnd() % (SPU_LS_SIZE / 16)) * 16;
        if (t % 4 == 0) from = 0xFFFFFFF0u; /* register and LS wrap together */
        if (t % 4 == 1) from = SPU_LS_SIZE - 32;
        ref.gpr[4]._u32[0] = from;
        ref.gpr[3]._u32[0] = from + bytes;
        if (t % 3 == 0) memset(&ref.gpr[5], 0, sizeof ref.gpr[5]);
        opt = ref; opt.ls = opt.ls_store;
        lifted_fill(&ref);
        gt5p_spu_fill_lines(&opt);
        assert(opt.gpr[4]._u32[0] == (bytes >= 64 ? from + bytes - 16 : from));
        lifted_fill(&opt);
        assert(!memcmp(ref.gpr, opt.gpr, sizeof ref.gpr));
        assert(!memcmp(ref.ls, opt.ls, SPU_LS_SIZE));
    }
    /* Invalid/unbounded shapes must fall back without attempting the loop. */
    const uint32_t spans[] = {0, 1, 15, 17, 31, SPU_LS_SIZE + 16, 0xFFFFFFF0u};
    for (unsigned i = 0; i < sizeof spans / sizeof *spans; i++) {
        memset(&opt, 0, sizeof opt); opt.ls = opt.ls_store;
        opt.gpr[4]._u32[0] = 0xFFF0; opt.gpr[3]._u32[0] = 0xFFF0 + spans[i]; unchanged();
    }
    memset(&opt, 0, sizeof opt); opt.ls = opt.ls_store; opt.gpr[3]._u32[0] = 256;
    opt.int_enable = opt.event_status = opt.event_mask = 1; unchanged(); opt.int_enable = 0;
    opt.policy_mode = 1; opt.image_id = 2; unchanged(); opt.policy_mode = 0;
    g_spu_ls_watch_n = 1; unchanged(); g_spu_ls_watch_n = 0;
    g_spu_smc_watch = 1; unchanged(); g_spu_smc_watch = 0;
    s_mode = 0; unchanged(); s_mode = 2; unchanged(); s_mode = 1;
    s_fill_mode = 0; unchanged();
    puts("native fill: 2400 full-LS/all-register comparisons, wraparound, guards and rollback PASS");
    s_fill_mode = 1;
    const unsigned sizes[] = {32, 256, 4096, 32768, SPU_LS_SIZE};
    for (int zero = 0; zero <= 1; zero++) for (unsigned k = 0; k < sizeof sizes / sizeof *sizes; k++) {
        double old = bench(original, sizes[k], zero), fast = bench(native, sizes[k], zero);
        printf("%u bytes %s: %.3f / %.3f us, %.2fx\n", sizes[k], zero ? "zero" : "pattern", old, fast, old / fast);
    }
}
