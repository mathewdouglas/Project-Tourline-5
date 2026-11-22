/*
 * gt5p_native_eiger.c -- native replacement for Eiger workload-0 grid pass
 * (b01697100 region 0x8298..0x8934, 42.8% of workload 0 in heavy rain).
 *
 * Jump hook at 0x8298 -> 0x8934 (config/targets.json spu_native_hooks,
 * scripts/spu_native_patch.py). Returns 1 when it completes the loop and
 * updates ctx->gpr[53] + local store; returns 0 to fall back to the lifted
 * region (e.g. pending interrupt, unexpected register layout, or
 * GT5P_NATIVE_EIGER=0).
 *
 * Bit-exact against gt5p_b01697100_region_00008298 (scripts/test/native_eiger.py).
 */
#pragma STDC FP_CONTRACT OFF
#include "spu_context.h"
#include "spu_helpers.h"
#include <immintrin.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_eiger_on = -1;
static int g_eiger_stats = 0;
static volatile long long g_eiger_hits = 0, g_eiger_bails = 0;
static __attribute__((noinline)) int eiger_init(void)
{
    const char* e = getenv("GT5P_NATIVE_EIGER");
    int on = !(e && e[0] == '0');
    const char* s = getenv("GT5P_NATIVE_EIGER_STATS");
    g_eiger_stats = (s && s[0] != '0');
#ifdef EIGER_FORCE_OFF
    on = 0;
#endif
    g_eiger_on = on;
    return on;
}

static inline uint32_t eg_be32(const uint8_t* p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return __builtin_bswap32(v);
}

static inline void eg_st_be32(uint8_t* p, uint32_t v)
{
    v = __builtin_bswap32(v);
    memcpy(p, &v, 4);
}

static inline float eg_be_f32(const uint8_t* p)
{
    uint32_t v = eg_be32(p);
    float f;
    memcpy(&f, &v, 4);
    return f;
}

static inline uint32_t eg_f2u(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

static inline float eg_u2f(uint32_t u)
{
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static __attribute__((target("sse4.1,fma"), always_inline)) inline float eg_fma_hw(float a, float b, float c)
{
    return _mm_cvtss_f32(_mm_fmadd_ss(_mm_set_ss(a), _mm_set_ss(b), _mm_set_ss(c)));
}

static __attribute__((target("sse4.1,fma"), always_inline)) inline float eg_fnms_hw(float a, float b, float c)
{
    return _mm_cvtss_f32(_mm_fnmadd_ss(_mm_set_ss(a), _mm_set_ss(b), _mm_set_ss(c)));
}

static inline float eg_fma_dbl(float a, float b, float c)
{
    return (float)((double)a * (double)b + (double)c);
}

static inline float eg_fnms_dbl(float a, float b, float c)
{
    return (float)((double)c - (double)a * (double)b);
}

static inline float eg_clamp(float x, float lo, float hi)
{
    float t = (x > lo) ? x : lo;
    return (hi > t) ? t : hi;
}

/* Scalar SPU frest + fi (bit-exact with spu_helpers.h SSE4.1 path). */
static inline float eg_frest_fi(float x)
{
    u128 v;
    v._f32[0] = x;
    v._u32[1] = v._u32[2] = v._u32[3] = 0x3F800000u;
    u128 e = spu_frest(v);
    return spu_fi(v, e)._f32[0];
}

/* Scalar SPU frsqest + fi (bit-exact with spu_helpers.h SSE4.1 path). */
static inline float eg_frsqest_fi(float x)
{
    u128 v;
    v._f32[0] = x;
    v._u32[1] = v._u32[2] = v._u32[3] = 0x3F800000u;
    u128 e = spu_frsqest(v);
    return spu_fi(v, e)._f32[0];
}

static inline int32_t eg_cflts_s(float x, int scale_exp)
{
    /* SPU cflts(a, imm): scale by 2^(173 - imm), truncate toward zero, saturate int32. */
    float sc = eg_u2f((uint32_t)(127 + (173 - scale_exp)) << 23);
    float t = x * sc;
    if (t >= 2147483648.0f) return 0x7FFFFFFF;
    if (t <= -2147483648.0f) return (int32_t)0x80000000u;
    return (int32_t)t;
}

static inline uint32_t eg_xorshift(uint32_t* s0, uint32_t* s1, uint32_t* s2, uint32_t* s3)
{
    uint32_t w0 = *s0, w1 = *s1, w2 = *s2, w3 = *s3;
    uint32_t t = (w3 ^ (w3 >> 8)) ^ (w0 ^ (w0 >> 19));
    *s0 = t;
    *s1 = w0;
    *s2 = w1;
    *s3 = w2 ^ (w2 << 11);
    return t;
}

/* Exact cuflt(u, 123) = (float)u * 2^-32 */
static inline float eg_cuflt_123(uint32_t u)
{
    return (float)u * 2.3283064365386962890625e-10f; /* 2^-32 */
}

/* Exact csflt(s, imm) = (float)s * 2^(imm - 155) */
static inline float eg_csflt_143(int32_t s) { return (float)s * 0.000244140625f; }      /* 2^-12 */
static inline float eg_csflt_131(int32_t s) { return (float)s * 5.9604644775390625e-8f; } /* 2^-24 */
static inline float eg_csflt_147(int32_t s) { return (float)s * 0.00390625f; }           /* 2^-8  */


#define EG_FMA  eg_fma_dbl
#define EG_FNMS eg_fnms_dbl
static __attribute__((noinline)) int eiger_8298_sse(spu_context* ctx)
{
    const uint32_t count = ctx->gpr[63]._u32[0];
    if (__builtin_expect(count == 0 || count > 65536u, 0))
        return 0;

    const uint32_t r1_0 = ctx->gpr[1]._u32[0];
    if (__builtin_expect((r1_0 & 15u) != 0, 0))
        return 0;

    /* Verify standard shuffle/bounds invariants from 0x8038..0x8238. */
    if (__builtin_expect(
            ctx->gpr[46]._u32[0] != 0 || ctx->gpr[46]._u32[1] != 0 ||
            (int32_t)ctx->gpr[46]._u32[2] < 0 || (int32_t)ctx->gpr[46]._u32[3] < 0 ||
            ctx->gpr[43]._u32[0] != 0x00010203u || ctx->gpr[43]._u32[1] != 0x14151617u ||
            ctx->gpr[45]._u32[0] != 0x10111213u || ctx->gpr[45]._u32[1] != 0x00010203u ||
            ctx->gpr[52]._u32[2] != 0x00010203u || ctx->gpr[59]._u32[3] != 0x00010203u ||
            ctx->gpr[69]._u32[0] != 0x00010203u || ctx->gpr[69]._u32[1] != 0x00010203u, 0))
        return 0;

    uint8_t* const ls = ctx->ls;
    const uint32_t row_tbl = (r1_0 + 0x320u) & SPU_LS_MASK;
    const uint32_t rng_addr = ctx->gpr[34]._u32[0] & (SPU_LS_MASK & ~15u);
    const uint32_t r66_0 = ctx->gpr[66]._u32[0];
    const uint32_t r126_0 = ctx->gpr[126]._u32[0];
    const uint32_t tbl_5000 = 0x5000u + r126_0;
    const uint32_t tbl_6000 = 0x6000u + r126_0;
    const uint32_t tbl_7000 = 0x7000u + r126_0;

    const u128 c_8e60 = spu_ls_read128(ctx, 0x8E60u);
    const uint8_t* const lut_8e70 = ls + 0x8E70u;
    const u128 c_8e80 = spu_ls_read128(ctx, 0x8E80u);

    const uint32_t mask_x = ctx->gpr[44]._u32[0];
    const uint32_t mask_y = ctx->gpr[44]._u32[1];
    const uint32_t max_x = ctx->gpr[46]._u32[2];
    const uint32_t max_y = ctx->gpr[46]._u32[3];

    const int32_t r54_1 = (int32_t)ctx->gpr[54]._u32[1];
    const int32_t r56_0 = (int32_t)ctx->gpr[56]._u32[0];
    const int32_t r56_1 = (int32_t)ctx->gpr[56]._u32[1];
    const float r57_0 = ctx->gpr[57]._f32[0];
    const float r58_0 = ctx->gpr[58]._f32[0], r58_1 = ctx->gpr[58]._f32[1];
    const float r61_0 = ctx->gpr[61]._f32[0], r61_1 = ctx->gpr[61]._f32[1];
    const float r62_0 = ctx->gpr[62]._f32[0], r62_1 = ctx->gpr[62]._f32[1];
    const float r62_2 = ctx->gpr[62]._f32[2], r62_3 = ctx->gpr[62]._f32[3];
    const float r64_0 = ctx->gpr[64]._f32[0], r64_1 = ctx->gpr[64]._f32[1];
    const float r65_0 = ctx->gpr[65]._f32[0], r65_1 = ctx->gpr[65]._f32[1];
    const int32_t r67_0 = (int32_t)ctx->gpr[67]._u32[0], r67_1 = (int32_t)ctx->gpr[67]._u32[1];
    const int32_t r70_0 = (int32_t)ctx->gpr[70]._u32[0], r70_1 = (int32_t)ctx->gpr[70]._u32[1];
    const float r71_0 = ctx->gpr[71]._f32[0];
    const float r72_0 = ctx->gpr[72]._f32[0];
    const float r73_0 = ctx->gpr[73]._f32[0];
    const float r74_0 = ctx->gpr[74]._f32[0], r74_1 = ctx->gpr[74]._f32[1];
    const float r75_0 = ctx->gpr[75]._f32[0];
    const float r76_0 = ctx->gpr[76]._f32[0];
    const float r77_0 = ctx->gpr[77]._f32[0];
    const float r78_0 = ctx->gpr[78]._f32[0];
    const float r82_0 = ctx->gpr[82]._f32[0], r82_1 = ctx->gpr[82]._f32[1];
    const float r82_2 = ctx->gpr[82]._f32[2];
    const float r83_0 = ctx->gpr[83]._f32[0], r83_1 = ctx->gpr[83]._f32[1];
    const float r83_2 = ctx->gpr[83]._f32[2];
    const float r84_0 = ctx->gpr[84]._f32[0], r84_1 = ctx->gpr[84]._f32[1];
    const float r84_2 = ctx->gpr[84]._f32[2];

    const float c_lo_v = eg_u2f(0xC0FCCCCCu); /* -7.8999996f */
    const float c_hi_v = eg_u2f(0x40FCCCCCu); /* +7.8999996f */
    const float c_lo_m = eg_u2f(0xBAF9096Bu);
    const float c_hi_m = eg_u2f(0x3AF9096Bu);
    const float c_eps  = eg_u2f(0x360637BDu);
    const float c_thr  = eg_u2f(0x38D1B717u); /* 1e-4f */

    /* Load xorshift128 RNG state into registers. */
    uint32_t rng0 = eg_be32(ls + rng_addr + 0);
    uint32_t rng1 = eg_be32(ls + rng_addr + 4);
    uint32_t rng2 = eg_be32(ls + rng_addr + 8);
    uint32_t rng3 = eg_be32(ls + rng_addr + 12);

    uint32_t r51_cur = ctx->gpr[51]._u32[0];
    float r53_0 = ctx->gpr[53]._f32[0], r53_1 = ctx->gpr[53]._f32[1];
    float r53_2 = ctx->gpr[53]._f32[2], r53_3 = ctx->gpr[53]._f32[3];

    for (uint32_t r55 = 0; r55 < count; r55++) {
        /* Load 8-byte cell entry at r66 + r55 * 8 (via quadword load + rotqby). */
        uint32_t c_addr = (r66_0 + ((uint32_t)(int32_t)(int16_t)(r55 & ~1u) << 3)) & (SPU_LS_MASK & ~15u);
        u128 r14_q = spu_ls_read128(ctx, c_addr);
        u128 r30_q = (r55 & 1u) ? spu_rotqbyi(r14_q, 8) : r14_q;

        /* Unpack 2D velocity r13 (words 0,1), scalar mass r9 (word 0), and r16 (words 0,1). */
        int32_t r8_0 = (int16_t)(r30_q._u32[0] & 0xFFFFu);
        int32_t r8_1 = (int16_t)(r30_q._u32[1] & 0xFFFFu);
        float r13_0 = eg_csflt_143(r8_0);
        float r13_1 = eg_csflt_143(r8_1);

        /* r31 = rotqmbyi(r30, -2) -> word 0 low 16 bits = high 16 bits of r30.w0?
         * Wait: rotqmbyi(r30, -2) shifts right by 2 bytes, so bytes 0..1 of r30 move to bytes 2..3 (word 0 low half)! */
        int32_t r29_0 = (int16_t)(r30_q._u32[0] >> 16);
        float r9_0 = eg_csflt_131(r29_0);

        u128 r28_q = spu_shufb(r30_q, r30_q, c_8e60);
        float r16_0 = eg_csflt_147((int32_t)r28_q._u32[0]);
        float r16_1 = eg_csflt_147((int32_t)r28_q._u32[1]);

        /* First xorshift128 step -> r27 (float in [0,1)) */
        uint32_t rnd1 = eg_xorshift(&rng0, &rng1, &rng2, &rng3);
        float r27_0 = eg_cuflt_123(rnd1);

        float v_sq = r13_0 * r13_0 + r13_1 * r13_1;
        uint32_t r15_tbl = (v_sq > r78_0) ? tbl_6000 : tbl_5000;
        int gt_zero = (r9_0 > 0.0f) ? 1 : 0;

        float r22_0;
        if (r77_0 > r27_0) {
            uint32_t rnd2 = eg_xorshift(&rng0, &rng1, &rng2, &rng3);
            float r39_0 = eg_cuflt_123(rnd2);
            float r4_0 = EG_FMA(r71_0, r39_0, r72_0);
            r22_0 = r9_0 + r4_0;
            float inv_r22 = eg_frest_fi(r22_0);
            float ratio = r9_0 * inv_r22;
            r13_0 *= ratio;
            r13_1 *= ratio;
            if (!gt_zero) {
                r16_0 = 0.5f;
                r16_1 = 0.5f;
            }
        } else {
            r22_0 = r9_0;
            if (!gt_zero) {
                goto next_outer;
            }
        }

        /* R_83D8 */
        {
            uint32_t p51_0 = r51_cur & (SPU_LS_MASK & ~15u);
            uint32_t p51_1 = (r51_cur + 0x10u) & (SPU_LS_MASK & ~15u);
            float r48_0 = eg_be_f32(ls + p51_0 + 0);
            float r48_1 = eg_be_f32(ls + p51_0 + 4);
            float r38_0 = eg_be_f32(ls + p51_1 + 0);
            float r38_1 = eg_be_f32(ls + p51_1 + 4);

            float r29_f = eg_frsqest_fi(r22_0);
            float r28_f = eg_frest_fi(r29_f);
            float r27_f = r73_0 * r28_f;

            float r10_sq = r13_0 * r13_0 + r13_1 * r13_1;
            float r28_sel = (c_thr > r10_sq) ? r76_0 : r75_0;

            float d0 = r38_0 - r13_0;
            float d1 = r38_1 - r13_1;
            float d_sq = d0 * d0 + d1 * d1;
            float r25_f = (d_sq > 0.0f) ? d_sq : 1.0f;
            float r24_s = r25_f * r27_f;
            float r60_f = eg_frsqest_fi(r25_f);

            float r49_0 = d0 * r60_f;
            float r49_1 = d1 * r60_f;
            float r47_0 = EG_FMA(r49_0, r24_s, r13_0);
            float r47_1 = EG_FMA(r49_1, r24_s, r13_1);
            float v_new0 = r47_0 + r48_0;
            float v_new1 = r47_1 + r48_1;
            /* r13 = {v_new0, v_new1, 0.0f, ...} */
            float n_sq = (v_new0 * v_new0 + v_new1 * v_new1) + 0.0f;
            float r21_f = (n_sq > 0.0f) ? n_sq : 1.0f;
            float r19_f = eg_frsqest_fi(r21_f);
            float r24_0 = v_new0 * r19_f;
            float r24_1 = v_new1 * r19_f;
            float r18_f = eg_frest_fi(r19_f);

            if (r18_f != 0.0f) {
                float r32_f = (r57_0 > r18_f) ? r18_f : r57_0;
                float r31_f = EG_FNMS(r32_f, 2.0f, r18_f);
                float r26_f = EG_FNMS(r28_sel, r29_f, r31_f);
                float r25_s = (r26_f > r57_0) ? r26_f : r57_0;
                r24_0 *= r25_s;
                r24_1 *= r25_s;
            }

            float sc0 = eg_clamp(r24_0 * r65_0, r61_0, r58_0);
            float sc1 = eg_clamp(r24_1 * r65_1, r61_1, r58_1);
            float r12_0 = EG_FMA(r16_0, r64_0, r53_0 + sc0);
            float r12_1 = EG_FMA(r16_1, r64_1, r53_1 + sc1);
            float r3_0 = r12_0 * r74_0;
            float r3_1 = r12_1 * r74_1;

            int32_t ix = eg_cflts_s(r3_0, 173);
            int32_t iy = eg_cflts_s(r3_1, 173);
            int32_t r42_0 = ix + r67_0;
            int32_t r42_1 = iy + r67_1;

            /* r37 = sf(r54, r42) = r42 - r54; r7 = r37 + {4, 4, 0, 0} */
            int32_t dx_c = (r42_0 - (int32_t)r55) + 4;
            int32_t dy_c = (r42_1 - r54_1) + 4;
            if (dx_c < 0) dx_c = 0;
            if (dy_c < 0) dy_c = 0;
            if (dx_c > r56_0) dx_c = r56_0;
            if (dy_c > r56_1) dy_c = r56_1;

            float frac0 = (r3_0 - (float)ix) + 0.0078125f; /* 0x3C000000 */
            float frac1 = (r3_1 - (float)iy) + 0.0078125f;
            int32_t q0 = eg_cflts_s(frac0, 165);
            int32_t q1 = eg_cflts_s(frac1, 165);
            if (q0 < 0) q0 = 0;
            if (q1 < 0) q1 = 0;
            if (q0 > r70_0) q0 = r70_0;
            if (q1 > r70_1) q1 = r70_1;

            uint32_t r29_off = (uint32_t)(dx_c + (dy_c << 3)) << 6;
            uint32_t r41_base = r15_tbl + r29_off;
            uint32_t r23_base = tbl_7000 + (uint32_t)((dx_c * 3) << 4) + (uint32_t)((dy_c * 3) << 7);

            /* Check inner loop count (low 16 bits of word 2 of ls[r41_base]). */
            uint32_t hdr_addr = r41_base & (SPU_LS_MASK & ~15u);
            int16_t inner_cnt = (int16_t)(eg_be32(ls + hdr_addr + 8) & 0xFFFFu);
            if (inner_cnt <= 0) {
                goto next_outer;
            }

            float r60_0 = eg_clamp(r24_0, c_lo_v, c_hi_v);
            float r60_1 = eg_clamp(r24_1, c_lo_v, c_hi_v);

            /* Pack r33_q = {q0, q1, 0, 0} for shufb(val, r33, c_8e80) */
            u128 r33_q;
            r33_q._u32[0] = (uint32_t)q0;
            r33_q._u32[1] = (uint32_t)q1;
            r33_q._u32[2] = 0;
            r33_q._u32[3] = 0;

            /* R_860C inner loop */
            uint32_t r29_in = 0;
            int16_t r31_in = 0;
            do {
                uint32_t ent_addr = (r41_base + r29_in) & (SPU_LS_MASK & ~15u);
                int32_t ex = (int32_t)eg_be32(ls + ent_addr + 0);
                int32_t ey = (int32_t)eg_be32(ls + ent_addr + 4);
                uint32_t flag = eg_be32(ls + ent_addr + 12);
                int32_t cx = (int32_t)((uint32_t)(ex + r42_0) & mask_x);
                int32_t cy = (int32_t)((uint32_t)(ey + r42_1) & mask_y);

                if (flag == 0u) {
                    /* Load 4 neighbor offsets from r23_base+0 (x) and r23_base+16 (y),
                     * and 4 weights from r23_base+32. */
                    uint32_t p23_0 = r23_base & (SPU_LS_MASK & ~15u);
                    uint32_t p23_1 = (r23_base + 0x10u) & (SPU_LS_MASK & ~15u);
                    uint32_t p23_2 = (r23_base + 0x20u) & (SPU_LS_MASK & ~15u);

                    uint32_t nx[4], ny[4];
                    float w[4];
                    for (int k = 0; k < 4; k++) {
                        nx[k] = (uint32_t)cx + eg_be32(ls + p23_0 + k * 4);
                        ny[k] = (uint32_t)cy + eg_be32(ls + p23_1 + k * 4);
                        uint32_t rptr = eg_be32(ls + ((row_tbl + (ny[k] << 2)) & SPU_LS_MASK));
                        uint32_t cell_a = (rptr + (nx[k] << 4)) & (SPU_LS_MASK & ~15u);
                        w[k] = eg_be_f32(ls + cell_a + 8);
                    }

                    uint32_t rnd3 = eg_xorshift(&rng0, &rng1, &rng2, &rng3);
                    float r17_f = eg_cuflt_123(rnd3);

                    float s0 = w[0] + 0.0f;
                    float s1 = s0 + w[1];
                    float s2 = s1 + w[2];
                    float s3 = s2 + w[3];
                    float thresh = (s3 + c_eps) * r17_f;

                    /* r15 = r5 + {w0, w1, w2, w3} (wait: r5 is ls[p23_2], r3 is {w0,w1,w2,w3})! */
                    float b0 = eg_be_f32(ls + p23_2 + 0) + w[0];
                    float b1 = eg_be_f32(ls + p23_2 + 4) + w[1];
                    float b2 = eg_be_f32(ls + p23_2 + 8) + w[2];
                    float b3 = eg_be_f32(ls + p23_2 + 12) + w[3];

                    /* r9 = fcgt(0.0f, b - thresh) -> bit (3 - k) set if (b[k] - thresh) < 0.0f */
                    uint32_t gb_mask = 0;
                    if (0.0f > (b0 - thresh)) gb_mask |= 8u;
                    if (0.0f > (b1 - thresh)) gb_mask |= 4u;
                    if (0.0f > (b2 - thresh)) gb_mask |= 2u;
                    if (0.0f > (b3 - thresh)) gb_mask |= 1u;

                    int pick = (int)(int8_t)lut_8e70[(3u + gb_mask) & 15u] & 3;
                    int32_t px = (int32_t)(nx[pick] & mask_x);
                    int32_t py = (int32_t)(ny[pick] & mask_y);

                    if ((uint32_t)px <= max_x && (uint32_t)py <= max_y) {
                        uint32_t rptr = eg_be32(ls + ((row_tbl + ((uint32_t)py << 2)) & SPU_LS_MASK));
                        uint32_t cell_a = (rptr + ((uint32_t)px << 4)) & (SPU_LS_MASK & ~15u);
                        float c0 = eg_be_f32(ls + cell_a + 0);
                        float c1 = eg_be_f32(ls + cell_a + 4);
                        float c2 = eg_be_f32(ls + cell_a + 8);

                        float n_m = eg_clamp(r22_0 + c2, c_lo_m, c_hi_m);
                        float inv_m = eg_frest_fi(n_m);
                        float nv0 = eg_clamp(EG_FMA(c2, c0, r22_0 * r24_0) * inv_m, c_lo_v, c_hi_v);
                        float nv1 = eg_clamp(EG_FMA(c2, c1, r22_0 * r24_1) * inv_m, c_lo_v, c_hi_v);

                        u128 r4_q;
                        r4_q._u32[0] = r4_q._u32[1] = r4_q._u32[3] = 0;
                        r4_q._f32[2] = n_m;
                        u128 r10_q = spu_shufb(r4_q, r33_q, c_8e80);
                        eg_st_be32(ls + cell_a + 0, eg_f2u(nv0));
                        eg_st_be32(ls + cell_a + 4, eg_f2u(nv1));
                        eg_st_be32(ls + cell_a + 8, r10_q._u32[2]);
                        eg_st_be32(ls + cell_a + 12, r10_q._u32[3]);
                    }
                } else if (flag == 1u) {
                    float evap = eg_clamp(r84_2 * r22_0, 0.0f, r83_2);
                    float rem = r22_0 - evap;
                    r22_0 = rem;
                    if ((uint32_t)cx <= max_x && (uint32_t)cy <= max_y) {
                        float m_w = eg_clamp(evap - r82_2, c_lo_m, c_hi_m);
                        float v_w = eg_clamp(0.0f, c_lo_v, c_hi_v);
                        u128 r2_q;
                        r2_q._u32[0] = r2_q._u32[1] = r2_q._u32[3] = 0;
                        r2_q._f32[2] = m_w;
                        u128 r6_q = spu_shufb(r2_q, r33_q, c_8e80);

                        uint32_t rptr = eg_be32(ls + ((row_tbl + ((uint32_t)cy << 2)) & SPU_LS_MASK));
                        uint32_t cell_a = (rptr + ((uint32_t)cx << 4)) & (SPU_LS_MASK & ~15u);
                        float old_c2 = eg_be_f32(ls + cell_a + 8);
                        eg_st_be32(ls + cell_a + 0, eg_f2u(v_w));
                        eg_st_be32(ls + cell_a + 4, eg_f2u(v_w));
                        eg_st_be32(ls + cell_a + 8, r6_q._u32[2]);
                        eg_st_be32(ls + cell_a + 12, r6_q._u32[3]);
                        r22_0 = rem + old_c2;
                    }
                } else {
                    if ((uint32_t)cx <= max_x && (uint32_t)cy <= max_y) {
                        uint32_t rptr = eg_be32(ls + ((row_tbl + ((uint32_t)cy << 2)) & SPU_LS_MASK));
                        uint32_t cell_a = (rptr + ((uint32_t)cx << 4)) & (SPU_LS_MASK & ~15u);
                        float old_c2 = eg_be_f32(ls + cell_a + 8);
                        if (!(old_c2 > 0.0f)) {
                            float m_w = eg_clamp(r22_0 - r82_2, c_lo_m, c_hi_m);
                            u128 r15_q;
                            r15_q._u32[0] = r15_q._u32[1] = r15_q._u32[3] = 0;
                            r15_q._f32[2] = m_w;
                            u128 r13_q = spu_shufb(r15_q, r33_q, c_8e80);
                            eg_st_be32(ls + cell_a + 0, eg_f2u(r60_0));
                            eg_st_be32(ls + cell_a + 4, eg_f2u(r60_1));
                            eg_st_be32(ls + cell_a + 8, r13_q._u32[2]);
                            eg_st_be32(ls + cell_a + 12, r13_q._u32[3]);
                        }
                    }
                }

                r31_in = (int16_t)(r31_in + 1);
                r29_in += 16u;
            } while (r31_in != inner_cnt);
        }

    next_outer:
        r53_0 += r62_0;
        r53_1 += r62_1;
        r53_2 += r62_2;
        r53_3 += r62_3;
        r51_cur += 32u;
    }

    /* Write back RNG state and live-out r53. */
    eg_st_be32(ls + rng_addr + 0, rng0);
    eg_st_be32(ls + rng_addr + 4, rng1);
    eg_st_be32(ls + rng_addr + 8, rng2);
    eg_st_be32(ls + rng_addr + 12, rng3);

    ctx->gpr[53]._f32[0] = r53_0;
    ctx->gpr[53]._f32[1] = r53_1;
    ctx->gpr[53]._f32[2] = r53_2;
    ctx->gpr[53]._f32[3] = r53_3;
    return 1;
}

#undef EG_FMA
#undef EG_FNMS

#define EG_FMA  eg_fma_hw
#define EG_FNMS eg_fnms_hw
static __attribute__((target("sse4.1,fma"), noinline)) int eiger_8298_fma(spu_context* ctx)
{
    const uint32_t count = ctx->gpr[63]._u32[0];
    if (__builtin_expect(count == 0 || count > 65536u, 0))
        return 0;

    const uint32_t r1_0 = ctx->gpr[1]._u32[0];
    if (__builtin_expect((r1_0 & 15u) != 0, 0))
        return 0;

    /* Verify standard shuffle/bounds invariants from 0x8038..0x8238. */
    if (__builtin_expect(
            ctx->gpr[46]._u32[0] != 0 || ctx->gpr[46]._u32[1] != 0 ||
            (int32_t)ctx->gpr[46]._u32[2] < 0 || (int32_t)ctx->gpr[46]._u32[3] < 0 ||
            ctx->gpr[43]._u32[0] != 0x00010203u || ctx->gpr[43]._u32[1] != 0x14151617u ||
            ctx->gpr[45]._u32[0] != 0x10111213u || ctx->gpr[45]._u32[1] != 0x00010203u ||
            ctx->gpr[52]._u32[2] != 0x00010203u || ctx->gpr[59]._u32[3] != 0x00010203u ||
            ctx->gpr[69]._u32[0] != 0x00010203u || ctx->gpr[69]._u32[1] != 0x00010203u, 0))
        return 0;

    uint8_t* const ls = ctx->ls;
    const uint32_t row_tbl = (r1_0 + 0x320u) & SPU_LS_MASK;
    const uint32_t rng_addr = ctx->gpr[34]._u32[0] & (SPU_LS_MASK & ~15u);
    const uint32_t r66_0 = ctx->gpr[66]._u32[0];
    const uint32_t r126_0 = ctx->gpr[126]._u32[0];
    const uint32_t tbl_5000 = 0x5000u + r126_0;
    const uint32_t tbl_6000 = 0x6000u + r126_0;
    const uint32_t tbl_7000 = 0x7000u + r126_0;

    const u128 c_8e60 = spu_ls_read128(ctx, 0x8E60u);
    const uint8_t* const lut_8e70 = ls + 0x8E70u;
    const u128 c_8e80 = spu_ls_read128(ctx, 0x8E80u);

    const uint32_t mask_x = ctx->gpr[44]._u32[0];
    const uint32_t mask_y = ctx->gpr[44]._u32[1];
    const uint32_t max_x = ctx->gpr[46]._u32[2];
    const uint32_t max_y = ctx->gpr[46]._u32[3];

    const int32_t r54_1 = (int32_t)ctx->gpr[54]._u32[1];
    const int32_t r56_0 = (int32_t)ctx->gpr[56]._u32[0];
    const int32_t r56_1 = (int32_t)ctx->gpr[56]._u32[1];
    const float r57_0 = ctx->gpr[57]._f32[0];
    const float r58_0 = ctx->gpr[58]._f32[0], r58_1 = ctx->gpr[58]._f32[1];
    const float r61_0 = ctx->gpr[61]._f32[0], r61_1 = ctx->gpr[61]._f32[1];
    const float r62_0 = ctx->gpr[62]._f32[0], r62_1 = ctx->gpr[62]._f32[1];
    const float r62_2 = ctx->gpr[62]._f32[2], r62_3 = ctx->gpr[62]._f32[3];
    const float r64_0 = ctx->gpr[64]._f32[0], r64_1 = ctx->gpr[64]._f32[1];
    const float r65_0 = ctx->gpr[65]._f32[0], r65_1 = ctx->gpr[65]._f32[1];
    const int32_t r67_0 = (int32_t)ctx->gpr[67]._u32[0], r67_1 = (int32_t)ctx->gpr[67]._u32[1];
    const int32_t r70_0 = (int32_t)ctx->gpr[70]._u32[0], r70_1 = (int32_t)ctx->gpr[70]._u32[1];
    const float r71_0 = ctx->gpr[71]._f32[0];
    const float r72_0 = ctx->gpr[72]._f32[0];
    const float r73_0 = ctx->gpr[73]._f32[0];
    const float r74_0 = ctx->gpr[74]._f32[0], r74_1 = ctx->gpr[74]._f32[1];
    const float r75_0 = ctx->gpr[75]._f32[0];
    const float r76_0 = ctx->gpr[76]._f32[0];
    const float r77_0 = ctx->gpr[77]._f32[0];
    const float r78_0 = ctx->gpr[78]._f32[0];
    const float r82_0 = ctx->gpr[82]._f32[0], r82_1 = ctx->gpr[82]._f32[1];
    const float r82_2 = ctx->gpr[82]._f32[2];
    const float r83_0 = ctx->gpr[83]._f32[0], r83_1 = ctx->gpr[83]._f32[1];
    const float r83_2 = ctx->gpr[83]._f32[2];
    const float r84_0 = ctx->gpr[84]._f32[0], r84_1 = ctx->gpr[84]._f32[1];
    const float r84_2 = ctx->gpr[84]._f32[2];

    const float c_lo_v = eg_u2f(0xC0FCCCCCu); /* -7.8999996f */
    const float c_hi_v = eg_u2f(0x40FCCCCCu); /* +7.8999996f */
    const float c_lo_m = eg_u2f(0xBAF9096Bu);
    const float c_hi_m = eg_u2f(0x3AF9096Bu);
    const float c_eps  = eg_u2f(0x360637BDu);
    const float c_thr  = eg_u2f(0x38D1B717u); /* 1e-4f */

    /* Load xorshift128 RNG state into registers. */
    uint32_t rng0 = eg_be32(ls + rng_addr + 0);
    uint32_t rng1 = eg_be32(ls + rng_addr + 4);
    uint32_t rng2 = eg_be32(ls + rng_addr + 8);
    uint32_t rng3 = eg_be32(ls + rng_addr + 12);

    uint32_t r51_cur = ctx->gpr[51]._u32[0];
    float r53_0 = ctx->gpr[53]._f32[0], r53_1 = ctx->gpr[53]._f32[1];
    float r53_2 = ctx->gpr[53]._f32[2], r53_3 = ctx->gpr[53]._f32[3];

    for (uint32_t r55 = 0; r55 < count; r55++) {
        /* Load 8-byte cell entry at r66 + r55 * 8 (via quadword load + rotqby). */
        uint32_t c_addr = (r66_0 + ((uint32_t)(int32_t)(int16_t)(r55 & ~1u) << 3)) & (SPU_LS_MASK & ~15u);
        u128 r14_q = spu_ls_read128(ctx, c_addr);
        u128 r30_q = (r55 & 1u) ? spu_rotqbyi(r14_q, 8) : r14_q;

        /* Unpack 2D velocity r13 (words 0,1), scalar mass r9 (word 0), and r16 (words 0,1). */
        int32_t r8_0 = (int16_t)(r30_q._u32[0] & 0xFFFFu);
        int32_t r8_1 = (int16_t)(r30_q._u32[1] & 0xFFFFu);
        float r13_0 = eg_csflt_143(r8_0);
        float r13_1 = eg_csflt_143(r8_1);

        /* r31 = rotqmbyi(r30, -2) -> word 0 low 16 bits = high 16 bits of r30.w0?
         * Wait: rotqmbyi(r30, -2) shifts right by 2 bytes, so bytes 0..1 of r30 move to bytes 2..3 (word 0 low half)! */
        int32_t r29_0 = (int16_t)(r30_q._u32[0] >> 16);
        float r9_0 = eg_csflt_131(r29_0);

        u128 r28_q = spu_shufb(r30_q, r30_q, c_8e60);
        float r16_0 = eg_csflt_147((int32_t)r28_q._u32[0]);
        float r16_1 = eg_csflt_147((int32_t)r28_q._u32[1]);

        /* First xorshift128 step -> r27 (float in [0,1)) */
        uint32_t rnd1 = eg_xorshift(&rng0, &rng1, &rng2, &rng3);
        float r27_0 = eg_cuflt_123(rnd1);

        float v_sq = r13_0 * r13_0 + r13_1 * r13_1;
        uint32_t r15_tbl = (v_sq > r78_0) ? tbl_6000 : tbl_5000;
        int gt_zero = (r9_0 > 0.0f) ? 1 : 0;

        float r22_0;
        if (r77_0 > r27_0) {
            uint32_t rnd2 = eg_xorshift(&rng0, &rng1, &rng2, &rng3);
            float r39_0 = eg_cuflt_123(rnd2);
            float r4_0 = EG_FMA(r71_0, r39_0, r72_0);
            r22_0 = r9_0 + r4_0;
            float inv_r22 = eg_frest_fi(r22_0);
            float ratio = r9_0 * inv_r22;
            r13_0 *= ratio;
            r13_1 *= ratio;
            if (!gt_zero) {
                r16_0 = 0.5f;
                r16_1 = 0.5f;
            }
        } else {
            r22_0 = r9_0;
            if (!gt_zero) {
                goto next_outer;
            }
        }

        /* R_83D8 */
        {
            uint32_t p51_0 = r51_cur & (SPU_LS_MASK & ~15u);
            uint32_t p51_1 = (r51_cur + 0x10u) & (SPU_LS_MASK & ~15u);
            float r48_0 = eg_be_f32(ls + p51_0 + 0);
            float r48_1 = eg_be_f32(ls + p51_0 + 4);
            float r38_0 = eg_be_f32(ls + p51_1 + 0);
            float r38_1 = eg_be_f32(ls + p51_1 + 4);

            float r29_f = eg_frsqest_fi(r22_0);
            float r28_f = eg_frest_fi(r29_f);
            float r27_f = r73_0 * r28_f;

            float r10_sq = r13_0 * r13_0 + r13_1 * r13_1;
            float r28_sel = (c_thr > r10_sq) ? r76_0 : r75_0;

            float d0 = r38_0 - r13_0;
            float d1 = r38_1 - r13_1;
            float d_sq = d0 * d0 + d1 * d1;
            float r25_f = (d_sq > 0.0f) ? d_sq : 1.0f;
            float r24_s = r25_f * r27_f;
            float r60_f = eg_frsqest_fi(r25_f);

            float r49_0 = d0 * r60_f;
            float r49_1 = d1 * r60_f;
            float r47_0 = EG_FMA(r49_0, r24_s, r13_0);
            float r47_1 = EG_FMA(r49_1, r24_s, r13_1);
            float v_new0 = r47_0 + r48_0;
            float v_new1 = r47_1 + r48_1;
            /* r13 = {v_new0, v_new1, 0.0f, ...} */
            float n_sq = (v_new0 * v_new0 + v_new1 * v_new1) + 0.0f;
            float r21_f = (n_sq > 0.0f) ? n_sq : 1.0f;
            float r19_f = eg_frsqest_fi(r21_f);
            float r24_0 = v_new0 * r19_f;
            float r24_1 = v_new1 * r19_f;
            float r18_f = eg_frest_fi(r19_f);

            if (r18_f != 0.0f) {
                float r32_f = (r57_0 > r18_f) ? r18_f : r57_0;
                float r31_f = EG_FNMS(r32_f, 2.0f, r18_f);
                float r26_f = EG_FNMS(r28_sel, r29_f, r31_f);
                float r25_s = (r26_f > r57_0) ? r26_f : r57_0;
                r24_0 *= r25_s;
                r24_1 *= r25_s;
            }

            float sc0 = eg_clamp(r24_0 * r65_0, r61_0, r58_0);
            float sc1 = eg_clamp(r24_1 * r65_1, r61_1, r58_1);
            float r12_0 = EG_FMA(r16_0, r64_0, r53_0 + sc0);
            float r12_1 = EG_FMA(r16_1, r64_1, r53_1 + sc1);
            float r3_0 = r12_0 * r74_0;
            float r3_1 = r12_1 * r74_1;

            int32_t ix = eg_cflts_s(r3_0, 173);
            int32_t iy = eg_cflts_s(r3_1, 173);
            int32_t r42_0 = ix + r67_0;
            int32_t r42_1 = iy + r67_1;

            /* r37 = sf(r54, r42) = r42 - r54; r7 = r37 + {4, 4, 0, 0} */
            int32_t dx_c = (r42_0 - (int32_t)r55) + 4;
            int32_t dy_c = (r42_1 - r54_1) + 4;
            if (dx_c < 0) dx_c = 0;
            if (dy_c < 0) dy_c = 0;
            if (dx_c > r56_0) dx_c = r56_0;
            if (dy_c > r56_1) dy_c = r56_1;

            float frac0 = (r3_0 - (float)ix) + 0.0078125f; /* 0x3C000000 */
            float frac1 = (r3_1 - (float)iy) + 0.0078125f;
            int32_t q0 = eg_cflts_s(frac0, 165);
            int32_t q1 = eg_cflts_s(frac1, 165);
            if (q0 < 0) q0 = 0;
            if (q1 < 0) q1 = 0;
            if (q0 > r70_0) q0 = r70_0;
            if (q1 > r70_1) q1 = r70_1;

            uint32_t r29_off = (uint32_t)(dx_c + (dy_c << 3)) << 6;
            uint32_t r41_base = r15_tbl + r29_off;
            uint32_t r23_base = tbl_7000 + (uint32_t)((dx_c * 3) << 4) + (uint32_t)((dy_c * 3) << 7);

            /* Check inner loop count (low 16 bits of word 2 of ls[r41_base]). */
            uint32_t hdr_addr = r41_base & (SPU_LS_MASK & ~15u);
            int16_t inner_cnt = (int16_t)(eg_be32(ls + hdr_addr + 8) & 0xFFFFu);
            if (inner_cnt <= 0) {
                goto next_outer;
            }

            float r60_0 = eg_clamp(r24_0, c_lo_v, c_hi_v);
            float r60_1 = eg_clamp(r24_1, c_lo_v, c_hi_v);

            /* Pack r33_q = {q0, q1, 0, 0} for shufb(val, r33, c_8e80) */
            u128 r33_q;
            r33_q._u32[0] = (uint32_t)q0;
            r33_q._u32[1] = (uint32_t)q1;
            r33_q._u32[2] = 0;
            r33_q._u32[3] = 0;

            /* R_860C inner loop */
            uint32_t r29_in = 0;
            int16_t r31_in = 0;
            do {
                uint32_t ent_addr = (r41_base + r29_in) & (SPU_LS_MASK & ~15u);
                int32_t ex = (int32_t)eg_be32(ls + ent_addr + 0);
                int32_t ey = (int32_t)eg_be32(ls + ent_addr + 4);
                uint32_t flag = eg_be32(ls + ent_addr + 12);
                int32_t cx = (int32_t)((uint32_t)(ex + r42_0) & mask_x);
                int32_t cy = (int32_t)((uint32_t)(ey + r42_1) & mask_y);

                if (flag == 0u) {
                    /* Load 4 neighbor offsets from r23_base+0 (x) and r23_base+16 (y),
                     * and 4 weights from r23_base+32. */
                    uint32_t p23_0 = r23_base & (SPU_LS_MASK & ~15u);
                    uint32_t p23_1 = (r23_base + 0x10u) & (SPU_LS_MASK & ~15u);
                    uint32_t p23_2 = (r23_base + 0x20u) & (SPU_LS_MASK & ~15u);

                    uint32_t nx[4], ny[4];
                    float w[4];
                    for (int k = 0; k < 4; k++) {
                        nx[k] = (uint32_t)cx + eg_be32(ls + p23_0 + k * 4);
                        ny[k] = (uint32_t)cy + eg_be32(ls + p23_1 + k * 4);
                        uint32_t rptr = eg_be32(ls + ((row_tbl + (ny[k] << 2)) & SPU_LS_MASK));
                        uint32_t cell_a = (rptr + (nx[k] << 4)) & (SPU_LS_MASK & ~15u);
                        w[k] = eg_be_f32(ls + cell_a + 8);
                    }

                    uint32_t rnd3 = eg_xorshift(&rng0, &rng1, &rng2, &rng3);
                    float r17_f = eg_cuflt_123(rnd3);

                    float s0 = w[0] + 0.0f;
                    float s1 = s0 + w[1];
                    float s2 = s1 + w[2];
                    float s3 = s2 + w[3];
                    float thresh = (s3 + c_eps) * r17_f;

                    /* r15 = r5 + {w0, w1, w2, w3} (wait: r5 is ls[p23_2], r3 is {w0,w1,w2,w3})! */
                    float b0 = eg_be_f32(ls + p23_2 + 0) + w[0];
                    float b1 = eg_be_f32(ls + p23_2 + 4) + w[1];
                    float b2 = eg_be_f32(ls + p23_2 + 8) + w[2];
                    float b3 = eg_be_f32(ls + p23_2 + 12) + w[3];

                    /* r9 = fcgt(0.0f, b - thresh) -> bit (3 - k) set if (b[k] - thresh) < 0.0f */
                    uint32_t gb_mask = 0;
                    if (0.0f > (b0 - thresh)) gb_mask |= 8u;
                    if (0.0f > (b1 - thresh)) gb_mask |= 4u;
                    if (0.0f > (b2 - thresh)) gb_mask |= 2u;
                    if (0.0f > (b3 - thresh)) gb_mask |= 1u;

                    int pick = (int)(int8_t)lut_8e70[(3u + gb_mask) & 15u] & 3;
                    int32_t px = (int32_t)(nx[pick] & mask_x);
                    int32_t py = (int32_t)(ny[pick] & mask_y);

                    if ((uint32_t)px <= max_x && (uint32_t)py <= max_y) {
                        uint32_t rptr = eg_be32(ls + ((row_tbl + ((uint32_t)py << 2)) & SPU_LS_MASK));
                        uint32_t cell_a = (rptr + ((uint32_t)px << 4)) & (SPU_LS_MASK & ~15u);
                        float c0 = eg_be_f32(ls + cell_a + 0);
                        float c1 = eg_be_f32(ls + cell_a + 4);
                        float c2 = eg_be_f32(ls + cell_a + 8);

                        float n_m = eg_clamp(r22_0 + c2, c_lo_m, c_hi_m);
                        float inv_m = eg_frest_fi(n_m);
                        float nv0 = eg_clamp(EG_FMA(c2, c0, r22_0 * r24_0) * inv_m, c_lo_v, c_hi_v);
                        float nv1 = eg_clamp(EG_FMA(c2, c1, r22_0 * r24_1) * inv_m, c_lo_v, c_hi_v);

                        u128 r4_q;
                        r4_q._u32[0] = r4_q._u32[1] = r4_q._u32[3] = 0;
                        r4_q._f32[2] = n_m;
                        u128 r10_q = spu_shufb(r4_q, r33_q, c_8e80);
                        eg_st_be32(ls + cell_a + 0, eg_f2u(nv0));
                        eg_st_be32(ls + cell_a + 4, eg_f2u(nv1));
                        eg_st_be32(ls + cell_a + 8, r10_q._u32[2]);
                        eg_st_be32(ls + cell_a + 12, r10_q._u32[3]);
                    }
                } else if (flag == 1u) {
                    float evap = eg_clamp(r84_2 * r22_0, 0.0f, r83_2);
                    float rem = r22_0 - evap;
                    r22_0 = rem;
                    if ((uint32_t)cx <= max_x && (uint32_t)cy <= max_y) {
                        float m_w = eg_clamp(evap - r82_2, c_lo_m, c_hi_m);
                        float v_w = eg_clamp(0.0f, c_lo_v, c_hi_v);
                        u128 r2_q;
                        r2_q._u32[0] = r2_q._u32[1] = r2_q._u32[3] = 0;
                        r2_q._f32[2] = m_w;
                        u128 r6_q = spu_shufb(r2_q, r33_q, c_8e80);

                        uint32_t rptr = eg_be32(ls + ((row_tbl + ((uint32_t)cy << 2)) & SPU_LS_MASK));
                        uint32_t cell_a = (rptr + ((uint32_t)cx << 4)) & (SPU_LS_MASK & ~15u);
                        float old_c2 = eg_be_f32(ls + cell_a + 8);
                        eg_st_be32(ls + cell_a + 0, eg_f2u(v_w));
                        eg_st_be32(ls + cell_a + 4, eg_f2u(v_w));
                        eg_st_be32(ls + cell_a + 8, r6_q._u32[2]);
                        eg_st_be32(ls + cell_a + 12, r6_q._u32[3]);
                        r22_0 = rem + old_c2;
                    }
                } else {
                    if ((uint32_t)cx <= max_x && (uint32_t)cy <= max_y) {
                        uint32_t rptr = eg_be32(ls + ((row_tbl + ((uint32_t)cy << 2)) & SPU_LS_MASK));
                        uint32_t cell_a = (rptr + ((uint32_t)cx << 4)) & (SPU_LS_MASK & ~15u);
                        float old_c2 = eg_be_f32(ls + cell_a + 8);
                        if (!(old_c2 > 0.0f)) {
                            float m_w = eg_clamp(r22_0 - r82_2, c_lo_m, c_hi_m);
                            u128 r15_q;
                            r15_q._u32[0] = r15_q._u32[1] = r15_q._u32[3] = 0;
                            r15_q._f32[2] = m_w;
                            u128 r13_q = spu_shufb(r15_q, r33_q, c_8e80);
                            eg_st_be32(ls + cell_a + 0, eg_f2u(r60_0));
                            eg_st_be32(ls + cell_a + 4, eg_f2u(r60_1));
                            eg_st_be32(ls + cell_a + 8, r13_q._u32[2]);
                            eg_st_be32(ls + cell_a + 12, r13_q._u32[3]);
                        }
                    }
                }

                r31_in = (int16_t)(r31_in + 1);
                r29_in += 16u;
            } while (r31_in != inner_cnt);
        }

    next_outer:
        r53_0 += r62_0;
        r53_1 += r62_1;
        r53_2 += r62_2;
        r53_3 += r62_3;
        r51_cur += 32u;
    }

    /* Write back RNG state and live-out r53. */
    eg_st_be32(ls + rng_addr + 0, rng0);
    eg_st_be32(ls + rng_addr + 4, rng1);
    eg_st_be32(ls + rng_addr + 8, rng2);
    eg_st_be32(ls + rng_addr + 12, rng3);

    ctx->gpr[53]._f32[0] = r53_0;
    ctx->gpr[53]._f32[1] = r53_1;
    ctx->gpr[53]._f32[2] = r53_2;
    ctx->gpr[53]._f32[3] = r53_3;
    return 1;
}

#undef EG_FMA
#undef EG_FNMS

int gt5p_eiger_8298(spu_context* ctx, int fused)
{
    int on = g_eiger_on;
    if (__builtin_expect(on < 0, 0)) on = eiger_init();
    if (!on) return 0;

    /* Bail if interrupt pending (match lifted trampoline check). */
    if (__builtin_expect(ctx->int_enable && (ctx->event_status & ctx->event_mask), 0))
        return 0;

    int ok = fused ? eiger_8298_fma(ctx) : eiger_8298_sse(ctx);
    if (__builtin_expect(g_eiger_stats, 0)) {
        long long n = ok ? __atomic_add_fetch(&g_eiger_hits, 1, __ATOMIC_RELAXED)
                         : __atomic_add_fetch(&g_eiger_bails, 1, __ATOMIC_RELAXED);
        if (n == 1 || (n & 0xFFFF) == 0) {
            fprintf(stderr, "[native-eiger] 0x8298 hits=%lld bails=%lld fused=%d\n",
                    g_eiger_hits, g_eiger_bails, fused);
        }
    }
    return ok;
}
