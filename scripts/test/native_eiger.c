/* Bit-exact test of gt5p_eiger_8298 against lifted gt5p_b01697100_region_00008298. */
#include "spu_context.h"
#include "spu_helpers.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Stubs needed by lifted region. */
volatile int g_spu_chain_back = 1;
unsigned g_spu_stack_reset_n = 0;
SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*) = NULL;
static void gt5p_b01697100_spu_func_00008298(spu_context* ctx) { (void)ctx; }
static void gt5p_b01697100_spu_func_0000860C(spu_context* ctx) { (void)ctx; }
static void gt5p_b01697100_spu_func_00008934(spu_context* ctx) { (void)ctx; }

static inline __m128i RX(u128 u) { __m128i v; memcpy(&v, &u, 16); return v; }
static inline u128 RU(__m128i v) { u128 r; memcpy(&r, &v, 16); return r; }

#include "eiger_8298.inc"
#include "gt5p_native_eiger.c"

static uint32_t xrnd(uint32_t* st)
{
    uint32_t x = *st;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *st = x;
    return x;
}

static void st_be32(uint8_t* p, uint32_t v)
{
    v = __builtin_bswap32(v);
    memcpy(p, &v, 4);
}

static void st_be_f32(uint8_t* p, float f)
{
    uint32_t v; memcpy(&v, &f, 4);
    st_be32(p, v);
}

int main(void)
{
    static spu_context a, b;
    static uint8_t ls_a[SPU_LS_SIZE], ls_b[SPU_LS_SIZE];
    uint32_t seed = 0x12345678u;
    int fused = 0;
#if defined(__FMA__)
    fused = 1;
#endif

    for (int tc = 0; tc < 200; tc++) {
        memset(&a, 0, sizeof(a));
        memset(ls_a, 0, sizeof(ls_a));
        a.ls = ls_a;
        /* Stack at 0x38000, row table at 0x38320, grid cells at 0x10000..0x20000 */
        const uint32_t sp = 0x38000u;
        const uint32_t row_tbl = sp + 0x320u;
        const uint32_t grid_w = 16u, grid_h = 16u;
        const uint32_t grid_base = 0x10000u;
        for (uint32_t y = 0; y < grid_h; y++) {
            st_be32(a.ls + row_tbl + y * 4u, grid_base + y * grid_w * 16u);
        }
        /* Fill grid cells with random floats */
        for (uint32_t i = 0; i < grid_w * grid_h; i++) {
            uint8_t* c = a.ls + grid_base + i * 16u;
            float vx = ((int32_t)(xrnd(&seed) % 200) - 100) * 0.05f;
            float vy = ((int32_t)(xrnd(&seed) % 200) - 100) * 0.05f;
            float m  = (xrnd(&seed) & 3) ? ((xrnd(&seed) % 100) * 0.00001f) : 0.0f;
            st_be_f32(c + 0, vx);
            st_be_f32(c + 4, vy);
            st_be_f32(c + 8, m);
            st_be32(c + 12, xrnd(&seed));
        }

        /* Constant tables at 0x8E60, 0x8E70, 0x8E80 */
        static const uint8_t t8e60[16] = {
            0x80,0x80,0x80,0x04, 0x80,0x80,0x80,0x05, 0x80,0x80,0x80,0x06, 0x80,0x80,0x80,0x07
        };
        static const uint8_t t8e70[16] = {
            0,1,2,3, 3,2,1,0, 1,2,3,0, 2,3,0,1
        };
        static const uint8_t t8e80[16] = {
            0x00,0x01,0x02,0x03, 0x04,0x05,0x06,0x07, 0x08,0x09,0x0A,0x0B, 0x13,0x17,0x80,0x80
        };
        memcpy(a.ls + 0x8E60, t8e60, 16);
        memcpy(a.ls + 0x8E70, t8e70, 16);
        memcpy(a.ls + 0x8E80, t8e80, 16);

        /* RNG state at 0x9000 */
        const uint32_t rng_addr = 0x9000u;
        for (int k = 0; k < 4; k++) st_be32(a.ls + rng_addr + k * 4, xrnd(&seed) | 1u);

        /* r126 = 0x18000 -> tables at 0x1D000, 0x1E000, 0x1F000 */
        const uint32_t r126 = 0x18000u;
        for (uint32_t t = 0; t < 2; t++) {
            uint32_t base = (t ? 0x6000u : 0x5000u) + r126;
            for (uint32_t idx = 0; idx < 64; idx++) {
                uint32_t e = base + idx * 64u;
                uint32_t n_in = (xrnd(&seed) % 4u); /* 0..3 inner entries */
                for (uint32_t j = 0; j < 4; j++) {
                    int32_t dx = (int32_t)(xrnd(&seed) % 3) - 1;
                    int32_t dy = (int32_t)(xrnd(&seed) % 3) - 1;
                    uint32_t fl = (xrnd(&seed) % 3); /* 0, 1, 2 */
                    st_be32(a.ls + e + j * 16 + 0, (uint32_t)dx);
                    st_be32(a.ls + e + j * 16 + 4, (uint32_t)dy);
                    st_be32(a.ls + e + j * 16 + 8, (j == 0) ? n_in : 0u);
                    st_be32(a.ls + e + j * 16 + 12, fl);
                }
            }
        }
        /* 0x7000 + r126 neighbor offset/weight tables */
        for (uint32_t idx = 0; idx < 64; idx++) {
            uint32_t p = 0x7000u + r126 + idx * 48u;
            if (p + 48u <= 0x38000u) {
                for (int k = 0; k < 4; k++) {
                    st_be32(a.ls + p + 0 + k * 4, (uint32_t)((int32_t)(xrnd(&seed) % 3) - 1));
                    st_be32(a.ls + p + 16 + k * 4, (uint32_t)((int32_t)(xrnd(&seed) % 3) - 1));
                    st_be_f32(a.ls + p + 32 + k * 4, (xrnd(&seed) % 100) * 0.0001f);
                }
            }
        }

        /* Input cell row at r66 = 0x28000, r51 = 0x2A000 */
        const uint32_t count = 1u + (xrnd(&seed) % 16u);
        const uint32_t r66 = 0x28000u;
        const uint32_t r51 = 0x2A000u;
        for (uint32_t i = 0; i < count; i++) {
            uint8_t* p = a.ls + r66 + i * 8u;
            st_be32(p + 0, xrnd(&seed));
            st_be32(p + 4, xrnd(&seed));
            uint8_t* q = a.ls + r51 + i * 32u;
            for (int k = 0; k < 8; k++)
                st_be_f32(q + k * 4, ((int32_t)(xrnd(&seed) % 200) - 100) * 0.02f);
        }

        a.gpr[1]._u32[0] = sp;
        a.gpr[34]._u32[0] = rng_addr;
        a.gpr[43] = spu_cwd(a.gpr[1], 0);
        a.gpr[44]._u32[0] = grid_w - 1; a.gpr[44]._u32[1] = grid_h - 1;
        a.gpr[44]._u32[2] = grid_w - 1; a.gpr[44]._u32[3] = grid_h - 1;
        a.gpr[45] = spu_cwd(a.gpr[1], 4);
        a.gpr[46]._u32[0] = 0; a.gpr[46]._u32[1] = 0;
        a.gpr[46]._u32[2] = grid_w - 1; a.gpr[46]._u32[3] = grid_h - 1;
        a.gpr[51]._u32[0] = r51;
        a.gpr[52] = spu_cwd(a.gpr[1], 8);
        a.gpr[53]._f32[0] = 1.0f; a.gpr[53]._f32[1] = 2.0f;
        a.gpr[54]._u32[1] = xrnd(&seed) % grid_h;
        a.gpr[55]._u32[0] = 0;
        a.gpr[56]._u32[0] = 7; a.gpr[56]._u32[1] = 7;
        a.gpr[57] = spu_fsmbi(0);
        for (int k = 0; k < 4; k++) {
            a.gpr[58]._f32[k] = 4.0f;
            a.gpr[61]._f32[k] = -4.0f;
            a.gpr[62]._f32[k] = 0.25f;
            a.gpr[64]._f32[k] = 0.5f;
            a.gpr[65]._f32[k] = 1.0f;
            a.gpr[67]._u32[k] = 1;
            a.gpr[70]._u32[k] = 255;
            a.gpr[71]._f32[k] = 0.0005f;
            a.gpr[72]._f32[k] = 0.0001f;
            a.gpr[73]._f32[k] = 0.1f;
            a.gpr[74]._f32[k] = 1.0f;
            a.gpr[75]._f32[k] = 0.02f;
            a.gpr[76]._f32[k] = 0.05f;
            a.gpr[77]._f32[k] = 0.5f;
            a.gpr[78]._f32[k] = 1.0f;
            a.gpr[82]._f32[k] = 0.00001f;
            a.gpr[83]._f32[k] = 0.001f;
            a.gpr[84]._f32[k] = 0.25f;
        }
        a.gpr[59] = spu_cwd(a.gpr[1], 12);
        a.gpr[63]._u32[0] = count;
        a.gpr[66]._u32[0] = r66;
        a.gpr[69] = spu_ila(0x10203);
        a.gpr[126]._u32[0] = r126;
        a.pc = 0x8298u;

        b = a;
        memcpy(ls_b, ls_a, sizeof(ls_a));
        b.ls = ls_b;
        gt5p_b01697100_region_00008298(&a);
        int ok = gt5p_eiger_8298(&b, fused);
        if (!ok) {
            fprintf(stderr, "FAIL tc=%d: native returned 0\n", tc);
            return 1;
        }
        if (memcmp(&a.gpr[53], &b.gpr[53], 16) != 0) {
            fprintf(stderr, "FAIL tc=%d: r53 mismatch\n", tc);
            return 1;
        }
        if (memcmp(a.ls, b.ls, SPU_LS_SIZE) != 0) {
            for (uint32_t off = 0; off < SPU_LS_SIZE; off++) {
                if (a.ls[off] != b.ls[off]) {
                    fprintf(stderr, "FAIL tc=%d: LS mismatch at 0x%05X (lifted=%02X native=%02X)\n",
                            tc, off, a.ls[off], b.ls[off]);
                    break;
                }
            }
            return 1;
        }
    }
    printf("PASS: 200 randomized Eiger 0x8298 states bit-exact (fused=%d)\n", fused);
    return 0;
}
