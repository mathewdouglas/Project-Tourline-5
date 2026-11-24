/* Exactness of runtime/ppu/ppu_vshuf.h: the SSE4.1 byte shuffles against the
 * scalar loops the PPU lifter emitted before (PPU_VSHUF=0). Built with
 * -msse4.1 by scripts/test/ppu_vshuf.py; exits non-zero on any difference. */
#include "ppu_vshuf.h"
#include <stdio.h>
#include <stdlib.h>

#if !PPU_VSHUF_SSE
#error "build with -msse4.1: this compares the SSE forms with the scalar ones"
#endif

static uint64_t s_rng = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17; return s_rng; }
static void fill(uint8_t* p, int n) { for (int i = 0; i < n; i++) p[i] = (uint8_t)rnd(); }

int main(void)
{
    unsigned long long n = 0, bad = 0;
    _Alignas(16) uint8_t a[16], b[16], c[16], d0[16], d1[16], m[16];

    /* vperm: random registers and selectors, every selector byte value in
     * every lane, and vD aliasing each source */
    for (int it = 0; it < 4000000; it++) {
        fill(a, 16); fill(b, 16); fill(c, 16);
        if (it < 256 * 16) c[it & 15] = (uint8_t)(it >> 4);
        ppu_vperm_scalar(d0, a, b, c); ppu_vperm(d1, a, b, c); n++;
        if (memcmp(d0, d1, 16)) { if (bad++ < 5) printf("vperm mismatch at %d\n", it); }
        uint8_t* srcs[3] = { a, b, c };
        uint8_t e0[3][16], e1[3][16];
        for (int k = 0; k < 3; k++) {
            uint8_t sa[16], sb[16], sc[16];
            memcpy(sa, a, 16); memcpy(sb, b, 16); memcpy(sc, c, 16);
            uint8_t* t[3] = { sa, sb, sc };
            ppu_vperm_scalar(t[k], sa, sb, sc); memcpy(e0[k], t[k], 16);
            memcpy(sa, a, 16); memcpy(sb, b, 16); memcpy(sc, c, 16);
            ppu_vperm(t[k], sa, sb, sc); memcpy(e1[k], t[k], 16);
            n++;
            if (memcmp(e0[k], e1[k], 16)) { if (bad++ < 5) printf("vperm alias %d mismatch at %d\n", k, it); }
        }
        (void)srcs;
    }
    /* lvlx / lvrx: every shift, random quadwords; lvsl/lvsr: every byte */
    for (int it = 0; it < 1000000; it++) {
        fill(m, 16);
        for (uint32_t sh = 0; sh < 16; sh++) {
            fill(d0, 16); memcpy(d1, d0, 16);
            ppu_lvlx_scalar(d0, m, sh); ppu_lvlx(d1, m, sh); n++;
            if (memcmp(d0, d1, 16)) { if (bad++ < 5) printf("lvlx sh=%u mismatch\n", sh); }
            fill(d0, 16); memcpy(d1, d0, 16);
            ppu_lvrx_scalar(d0, m, sh); ppu_lvrx(d1, m, sh); n++;
            if (memcmp(d0, d1, 16)) { if (bad++ < 5) printf("lvrx sh=%u mismatch\n", sh); }
        }
    }
    for (int v = 0; v < 256; v++) {
        ppu_lvsl_scalar(d0, (uint8_t)v); ppu_lvsl(d1, (uint8_t)v); n++;
        if (memcmp(d0, d1, 16)) { if (bad++ < 5) printf("lvsl %d mismatch\n", v); }
    }
    printf("ppu_vshuf: %llu comparisons, %llu differences\n", n, bad);
    return bad ? 1 : 0;
}
