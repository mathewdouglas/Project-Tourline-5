/* Driver for scripts/test/spu_regpromo.py: original against promoted lifted code. */
#include "spu_helpers.h"
#include <assert.h>
#include <stdio.h>
volatile int g_spu_fast = 1;
volatile int g_spu_chain_back = 1;
unsigned g_spu_stack_reset_n;
int g_spu_ls_watch_n;
void spu_ls_watch_slow(uint32_t a, int b, const uint8_t* p, uint32_t pc, uint32_t lr) { abort(); }
#include "../../src/gt5p_spu_native.c"
#include "pairs.inc"

typedef void (*fn)(spu_context*);
static const struct { fn oc, rpc; uint32_t block; const char* image; } pairs[] = { RP_PAIRS };

static uint32_t seed = 0x20261111;
static uint32_t rnd(void) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; }
/* Finite, |x| < 2 as a float: no NaN or infinity in any input word. */
static uint32_t word(void) { return rnd() & ~0x40000000u; }

static int same_words(const uint32_t* a, const uint32_t* b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (a[i] == b[i]) continue;
        const int na = (a[i] & 0x7F800000u) == 0x7F800000u && (a[i] & 0x7FFFFFu);
        const int nb = (b[i] & 0x7F800000u) == 0x7F800000u && (b[i] & 0x7FFFFFu);
        if (na && nb) continue;
        return 0;
    }
    return 1;
}

static spu_context a, b, keep;

int main(void)
{
    _putenv("GT5P_SPU_REGPROMO_CHECK_EVERY=1");
    unsigned cases = 0, guarded = 0;
    for (size_t p = 0; p < sizeof pairs / sizeof pairs[0]; p++) {
        for (unsigned t = 0; t < 100; t++) {
            memset(&a, 0, sizeof a); a.ls = a.ls_store;
            for (unsigned i = 0; i < SPU_LS_SIZE; i += 4) { uint32_t v = word(); memcpy(a.ls + i, &v, 4); }
            for (unsigned r = 0; r < 128; r++) for (unsigned k = 0; k < 4; k++) a.gpr[r]._u32[k] = word();
            a.pc = pairs[p].block; a.int_enable = t & 1;
            b = a; b.ls = b.ls_store;
            pairs[p].oc(&a);
            pairs[p].rpc(&b);
            guarded += a.pc == 0x3FFFCu;
            if (a.pc != b.pc || a.int_enable != b.int_enable ||
                !same_words(&a.gpr[0]._u32[0], &b.gpr[0]._u32[0], 128 * 4) ||
                !same_words((const uint32_t*)a.ls, (const uint32_t*)b.ls, SPU_LS_SIZE / 4)) {
                fprintf(stderr, "%s 0x%X case %u: promoted copy differs (pc %05X/%05X)\n",
                        pairs[p].image, pairs[p].block, t, a.pc, b.pc);
                return 1;
            }
            cases++;
        }
        /* The game's check routine: compares, then leaves the state as it found it. */
        keep = a; keep.ls = keep.ls_store; memcpy(keep.ls, a.ls, SPU_LS_SIZE);
        gt5p_spu_regpromo_check(&a, pairs[p].oc, pairs[p].rpc, pairs[p].block);
        assert(!memcmp(keep.gpr, a.gpr, sizeof a.gpr) && keep.pc == a.pc && keep.int_enable == a.int_enable);
        assert(!memcmp(keep.ls, a.ls, SPU_LS_SIZE));
    }
    printf("register promotion: %zu functions, %u cases agree (%u stopped by the loop guard); "
           "check routine retains state, %lld differences counted\n",
           sizeof pairs / sizeof pairs[0], cases, guarded, s_rp_bad);
    return s_rp_bad ? 1 : 0;
}
