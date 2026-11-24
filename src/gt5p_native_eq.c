/*
 * gt5p_native_eq.c -- native fast blocks for the FFT butterfly loops of GT5's EQ / loudness SPU
 * job (socepdideq, image jEB79FDFC): 0x3C6C, 0x3ADC, 0x4158, 0x4008, 0x3D80, 0x43C0.
 *
 * The fast blocks and their helpers are gt5-recomp's (MIT License, Copyright (c) 2026
 * MormonFTP-hub, GT5/src/native/native_jEB79FDFC.c and native_snd.h, v0.1.92). Changed for
 * this runtime: its lifted fa/fs/fm/fma do not turn -0 into +0, so a stored value is the host
 * result as it is (NS_KEEP; gt5-recomp's runtime flushes there); the LS accessor; the hooks.
 *
 * Scheme (as the mixer, src/gt5p_native_mixer.c): each block advances its lifted loop by whole
 * passes, writing only the loop-carried registers and the lines those passes store, then the
 * lifted function finishes. A pass with any lane outside the host float range (or a twiddle
 * double that is not finite) is left to the lifted code. Only the FMA copy of the image is hooked
 * (audio_native_hooks). GT5P_NATIVE_EQ=0: every call goes straight to the lifted function.
 * Test: scripts/test/native_eq.py (captured states, native against lifted).
 */
#include "spu_recomp.h"
#include "spu_helpers.h"
#include <immintrin.h>
#include <stdlib.h>
#include <string.h>

#if !defined(__FMA__) || !defined(__SSE4_1__)
#error "gt5p_native_eq.c is built like the FMA copy of the image: -msse4.1 -mfma"
#endif

#ifndef NS_PREFIX
#define NS_PREFIX gt5p_audio_jEB79FDFC_spu_func_
#endif
#define NS_CAT2(a, b) a##b
#define NS_CAT(a, b) NS_CAT2(a, b)
#define NATIVE_FN(A) NS_CAT(NS_PREFIX, A)
#define ORIG_FN(A)   NS_CAT(NS_CAT(NS_PREFIX, A), _orig)

#define NS_INLINE static inline __attribute__((always_inline))
#define NS_QMASK  (SPU_LS_MASK & ~0xFu)
#define NS_STAT(fn, field, n) ((void)0)
#define NS_KEEP(x) (x)

/* ---- helpers of the fast blocks --------------------------------------------------------- */
/* local store is big-endian; host lane i = SPU word i */
NS_INLINE __m128i ns_bswap(__m128i x) {
    return _mm_shuffle_epi8(x, _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12));
}
NS_INLINE uint8_t* ns_line(uint8_t* ls, uint32_t lsa) { return ls + (lsa & NS_QMASK); }
NS_INLINE __m128 ns_ldq(const uint8_t* p) { return _mm_castsi128_ps(ns_bswap(_mm_loadu_si128((const __m128i*)p))); }
NS_INLINE void ns_stq(uint8_t* p, __m128 v) { _mm_storeu_si128((__m128i*)p, ns_bswap(_mm_castps_si128(v))); }
NS_INLINE __m128 ns_reg(u128 a) { return _mm_loadu_ps((const float*)a._u32); }
NS_INLINE u128 ns_u128(__m128 v) { u128 r; _mm_storeu_ps((float*)r._u32, v); return r; }
NS_INLINE u128 ns_splat_add(u128 r, uint32_t d) { for (int i = 0; i < 4; i++) r._u32[i] += d; return r; }

/* Range bookkeeping: NS_ACC folds |t| (as an integer) into a running maximum; ns_bad says whether
 * any folded lane failed the lifted helpers' test |t| < FLT_MAX, i.e. is FLT_MAX, infinity or NaN. */
NS_INLINE __m128i ns_mag(__m128 t) { return _mm_and_si128(_mm_castps_si128(t), _mm_set1_epi32(0x7FFFFFFF)); }
#define NS_ACC(acc, t) ((acc) = _mm_max_epu32((acc), ns_mag(t)))
NS_INLINE int ns_bad(__m128i acc) {
    return _mm_movemask_ps(_mm_castsi128_ps(_mm_cmpgt_epi32(acc, _mm_set1_epi32(0x7F7FFFFE)))) != 0;
}
/* a register holding a shufb control in a known pattern */
NS_INLINE int ns_is(u128 r, uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3) {
    return r._u32[0] == w0 && r._u32[1] == w1 && r._u32[2] == w2 && r._u32[3] == w3;
}

/* Doubles: SPU doubleword i = (_u32[2i] << 32) | _u32[2i+1]; as a host __m128d the two 32-bit
 * halves of each doubleword are swapped. */
NS_INLINE __m128d ns_dreg(u128 a) {
    return _mm_castsi128_pd(_mm_shuffle_epi32(_mm_loadu_si128((const __m128i*)a._u32), _MM_SHUFFLE(2, 3, 0, 1)));
}
NS_INLINE u128 ns_du128(__m128d v) {
    u128 r; _mm_storeu_si128((__m128i*)r._u32, _mm_shuffle_epi32(_mm_castpd_si128(v), _MM_SHUFFLE(2, 3, 0, 1))); return r;
}
/* both doubles finite (exponent below 0x7FF) */
NS_INLINE int ns_dfinite(__m128d v) {
    const __m128i e = _mm_set1_epi64x(0x7FF0000000000000ll);
    return _mm_movemask_pd(_mm_castsi128_pd(_mm_cmpeq_epi64(_mm_and_si128(_mm_castpd_si128(v), e), e))) == 0;
}

#define CTL_SPLAT0(r) ns_is(r, 0x00010203u, 0x00010203u, 0x00010203u, 0x00010203u)

typedef struct ns_tw { __m128d w, alpha, beta; __m128i mask; __m128 c, s; } ns_tw;
NS_INLINE int ns_tw_init(ns_tw* t, u128 w, u128 alpha, u128 beta, u128 mask) {
    t->w = ns_dreg(w); t->alpha = ns_dreg(alpha); t->beta = ns_dreg(beta);
    /* the mask is applied to swap(w) as a register xor: bring it into the host doubles' layout */
    t->mask = _mm_castpd_si128(ns_dreg(mask));
    return ns_dfinite(t->w) && ns_dfinite(t->alpha) && ns_dfinite(t->beta);
}
/* c, s of this pass and the next w; 0 when the pass has to be left to the lifted code */
NS_INLINE int ns_tw_step(ns_tw* t, __m128d* wn) {
    const __m128 cs = _mm_cvtpd_ps(t->w);                                   /* frds: c, s, 0, 0 */
    t->c = _mm_shuffle_ps(cs, cs, _MM_SHUFFLE(0, 0, 0, 0));
    t->s = _mm_shuffle_ps(cs, cs, _MM_SHUFFLE(1, 1, 1, 1));
    const __m128d sw = _mm_castsi128_pd(_mm_xor_si128(_mm_castpd_si128(_mm_shuffle_pd(t->w, t->w, 1)), t->mask));
    const __m128d w1 = _mm_fmadd_pd(t->w, t->alpha, t->w);
    *wn = _mm_fmadd_pd(t->beta, sw, w1);
    return ns_dfinite(sw);
}

/* ---------------------------------------------------------------------------------------------
 * 0x3C6C: forward FFT, the stages after the first; in place on four streams.
 *     A = LS[r19] (2 lines)  B = LS[r17]  C = LS[r20]  D = LS[r18]
 *     C' = C + D   A' = A + B   D' = (C-D)*c - (A-B)*s   B' = (A-B)*c + (C-D)*s
 * twiddle w = r21, alpha = r24, beta = r26, mask = r23; splat control r25; count r22.
 * Carried: r17 r18 r19 r20 r21 r22.
 * ------------------------------------------------------------------------------------------- */
static void ns_fast_00003C6C(spu_context* ctx)
{
    u128* const g = ctx->gpr;
    uint8_t* const ls = ctx->ls;
    const uint32_t n = g[22]._u32[0];
    ns_tw t;
    if (n < 2 || n > 0x4000 || !CTL_SPLAT0(g[25]) || !ns_tw_init(&t, g[21], g[24], g[26], g[23])) {
        NS_STAT(NS_EB_3C6C, bail_precond, 1); return;
    }
    const uint32_t pa = g[19]._u32[0], pb = g[17]._u32[0], pc = g[20]._u32[0], pd = g[18]._u32[0];
    uint32_t k = 0;
    for (; k < n - 1; k++) {
        __m128d wn;
        if (__builtin_expect(!ns_tw_step(&t, &wn), 0)) break;
        const uint32_t o = 32u * k;
        uint8_t* const a0p = ns_line(ls, pa + o), * const a1p = ns_line(ls, pa + o + 16);
        uint8_t* const b0p = ns_line(ls, pb + o), * const b1p = ns_line(ls, pb + o + 16);
        uint8_t* const c0p = ns_line(ls, pc + o), * const c1p = ns_line(ls, pc + o + 16);
        uint8_t* const d0p = ns_line(ls, pd + o), * const d1p = ns_line(ls, pd + o + 16);
        const __m128 A0 = ns_ldq(a0p), A1 = ns_ldq(a1p), B0 = ns_ldq(b0p), B1 = ns_ldq(b1p);
        const __m128 C0 = ns_ldq(c0p), C1 = ns_ldq(c1p), D0 = ns_ldq(d0p), D1 = ns_ldq(d1p);
        const __m128 ab0 = _mm_sub_ps(A0, B0), ab1 = _mm_sub_ps(A1, B1), sa0 = _mm_add_ps(A0, B0), sa1 = _mm_add_ps(A1, B1);
        const __m128 cd0 = _mm_sub_ps(C0, D0), cd1 = _mm_sub_ps(C1, D1), sc0 = _mm_add_ps(C0, D0), sc1 = _mm_add_ps(C1, D1);
        const __m128 m0 = _mm_mul_ps(ab0, t.s), m1 = _mm_mul_ps(ab1, t.s), m2 = _mm_mul_ps(cd0, t.s), m3 = _mm_mul_ps(cd1, t.s);
        const __m128 dn0 = _mm_fmsub_ps(cd0, t.c, m0), dn1 = _mm_fmsub_ps(cd1, t.c, m1);
        const __m128 bn0 = _mm_fmadd_ps(ab0, t.c, m2), bn1 = _mm_fmadd_ps(ab1, t.c, m3);
        __m128i acc = ns_mag(ab0);
        NS_ACC(acc, ab1); NS_ACC(acc, sa0); NS_ACC(acc, sa1); NS_ACC(acc, cd0); NS_ACC(acc, cd1); NS_ACC(acc, sc0); NS_ACC(acc, sc1);
        NS_ACC(acc, m0); NS_ACC(acc, m1); NS_ACC(acc, m2); NS_ACC(acc, m3);
        NS_ACC(acc, dn0); NS_ACC(acc, dn1); NS_ACC(acc, bn0); NS_ACC(acc, bn1);
        if (__builtin_expect(ns_bad(acc), 0)) break;
        ns_stq(c0p, NS_KEEP(sc0)); ns_stq(c1p, NS_KEEP(sc1));
        ns_stq(a0p, NS_KEEP(sa0)); ns_stq(a1p, NS_KEEP(sa1));
        ns_stq(d0p, NS_KEEP(dn0)); ns_stq(d1p, NS_KEEP(dn1));
        ns_stq(b0p, NS_KEEP(bn0)); ns_stq(b1p, NS_KEEP(bn1));
        t.w = wn;
    }
    if (k) {
        g[17] = ns_splat_add(g[17], 32u * k); g[18] = ns_splat_add(g[18], 32u * k);
        g[19] = ns_splat_add(g[19], 32u * k); g[20] = ns_splat_add(g[20], 32u * k);
        g[21] = ns_du128(t.w);
        g[22] = ns_splat_add(g[22], 0u - k);
        NS_STAT(NS_EB_3C6C, fast_calls, 1); NS_STAT(NS_EB_3C6C, fast_iters, k);
    }
    if (k != n - 1) NS_STAT(NS_EB_3C6C, bail_range, 1);
}

/* ---------------------------------------------------------------------------------------------
 * 0x3ADC: forward FFT, first stage: reads two streams (r17 forwards 4 lines a pass, r18 backwards),
 * writes four (r22, r21, r20, r23, 2 lines a pass each).
 *     P = LS[r17], LS[r17+16]   Q = LS[r18], LS[r18+16]   R = LS[r17+32], +48   T = LS[r18+32], +48
 *     LS[r22] = P + Q     LS[r21] = R - T
 *     LS[r20] = (P-Q)*c - (R+T)*s     LS[r23] = (R+T)*c + (P-Q)*s
 * twiddle w = r19, alpha = r28, beta = r26, mask = r25; splat control r27; count r24.
 * Carried: r17 r18 r19 r20 r21 r22 r23 r24.
 * ------------------------------------------------------------------------------------------- */
static void ns_fast_00003ADC(spu_context* ctx)
{
    u128* const g = ctx->gpr;
    uint8_t* const ls = ctx->ls;
    const uint32_t n = g[24]._u32[0];
    ns_tw t;
    if (n < 2 || n > 0x4000 || !CTL_SPLAT0(g[27]) || !ns_tw_init(&t, g[19], g[28], g[26], g[25])) {
        NS_STAT(NS_EB_3ADC, bail_precond, 1); return;
    }
    const uint32_t p17 = g[17]._u32[0], p18 = g[18]._u32[0], p20 = g[20]._u32[0], p21 = g[21]._u32[0],
                   p22 = g[22]._u32[0], p23 = g[23]._u32[0];
    uint32_t k = 0;
    for (; k < n - 1; k++) {
        __m128d wn;
        if (__builtin_expect(!ns_tw_step(&t, &wn), 0)) break;
        const uint32_t i17 = p17 + 64u * k, i18 = p18 - 64u * k, o = 32u * k;
        const __m128 R0 = ns_ldq(ns_line(ls, i17 + 0x20)), R1 = ns_ldq(ns_line(ls, i17 + 0x30));
        const __m128 T0 = ns_ldq(ns_line(ls, i18 + 0x20)), T1 = ns_ldq(ns_line(ls, i18 + 0x30));
        const __m128 P0 = ns_ldq(ns_line(ls, i17)), P1 = ns_ldq(ns_line(ls, i17 + 0x10));
        const __m128 Q0 = ns_ldq(ns_line(ls, i18)), Q1 = ns_ldq(ns_line(ls, i18 + 0x10));
        const __m128 rt0 = _mm_add_ps(R0, T0), rt1 = _mm_add_ps(R1, T1), dr0 = _mm_sub_ps(R0, T0), dr1 = _mm_sub_ps(R1, T1);
        const __m128 pq0 = _mm_sub_ps(P0, Q0), pq1 = _mm_sub_ps(P1, Q1), sp0 = _mm_add_ps(P0, Q0), sp1 = _mm_add_ps(P1, Q1);
        const __m128 m0 = _mm_mul_ps(rt0, t.s), m1 = _mm_mul_ps(rt1, t.s), m2 = _mm_mul_ps(pq0, t.s), m3 = _mm_mul_ps(pq1, t.s);
        const __m128 e0 = _mm_fmsub_ps(pq0, t.c, m0), e1 = _mm_fmsub_ps(pq1, t.c, m1);
        const __m128 f0 = _mm_fmadd_ps(rt0, t.c, m2), f1 = _mm_fmadd_ps(rt1, t.c, m3);
        __m128i acc = ns_mag(rt0);
        NS_ACC(acc, rt1); NS_ACC(acc, dr0); NS_ACC(acc, dr1); NS_ACC(acc, pq0); NS_ACC(acc, pq1); NS_ACC(acc, sp0); NS_ACC(acc, sp1);
        NS_ACC(acc, m0); NS_ACC(acc, m1); NS_ACC(acc, m2); NS_ACC(acc, m3);
        NS_ACC(acc, e0); NS_ACC(acc, e1); NS_ACC(acc, f0); NS_ACC(acc, f1);
        if (__builtin_expect(ns_bad(acc), 0)) break;
        ns_stq(ns_line(ls, p22 + o), NS_KEEP(sp0)); ns_stq(ns_line(ls, p22 + o + 16), NS_KEEP(sp1));
        ns_stq(ns_line(ls, p21 + o), NS_KEEP(dr0)); ns_stq(ns_line(ls, p21 + o + 16), NS_KEEP(dr1));
        ns_stq(ns_line(ls, p20 + o), NS_KEEP(e0));  ns_stq(ns_line(ls, p20 + o + 16), NS_KEEP(e1));
        ns_stq(ns_line(ls, p23 + o), NS_KEEP(f0));  ns_stq(ns_line(ls, p23 + o + 16), NS_KEEP(f1));
        t.w = wn;
    }
    if (k) {
        g[17] = ns_splat_add(g[17], 64u * k); g[18] = ns_splat_add(g[18], 0u - 64u * k);
        g[20] = ns_splat_add(g[20], 32u * k); g[21] = ns_splat_add(g[21], 32u * k);
        g[22] = ns_splat_add(g[22], 32u * k); g[23] = ns_splat_add(g[23], 32u * k);
        g[19] = ns_du128(t.w);
        g[24] = ns_splat_add(g[24], 0u - k);
        NS_STAT(NS_EB_3ADC, fast_calls, 1); NS_STAT(NS_EB_3ADC, fast_iters, k);
    }
    if (k != n - 1) NS_STAT(NS_EB_3ADC, bail_range, 1);
}

/* ---------------------------------------------------------------------------------------------
 * 0x4158: inverse FFT, the stages after the first; in place on two streams, 4 lines a pass each.
 *     U = LS[r18 .. +0x30]   V = LS[r17 .. +0x30]         d_j = U_j - V_j
 *     U_j' = U_j + V_j
 *     V0' = d0*c - d2*s   V1' = d1*c - d3*s   V2' = d2*c + d0*s   V3' = d3*c + d1*s
 * twiddle w = r19, alpha = r22, beta = r24, mask = r21; splat control r23; count r20.
 * Carried: r17 r18 r19 r20.
 * ------------------------------------------------------------------------------------------- */
static void ns_fast_00004158(spu_context* ctx)
{
    u128* const g = ctx->gpr;
    uint8_t* const ls = ctx->ls;
    const uint32_t n = g[20]._u32[0];
    ns_tw t;
    if (n < 2 || n > 0x4000 || !CTL_SPLAT0(g[23]) || !ns_tw_init(&t, g[19], g[22], g[24], g[21])) {
        NS_STAT(NS_EB_4158, bail_precond, 1); return;
    }
    const uint32_t pu = g[18]._u32[0], pv = g[17]._u32[0];
    uint32_t k = 0;
    for (; k < n - 1; k++) {
        __m128d wn;
        if (__builtin_expect(!ns_tw_step(&t, &wn), 0)) break;
        const uint32_t o = 64u * k;
        uint8_t* up[4]; uint8_t* vp[4];
        __m128 U[4], V[4], d[4], sum[4];
        for (int j = 0; j < 4; j++) { up[j] = ns_line(ls, pu + o + 16u * j); vp[j] = ns_line(ls, pv + o + 16u * j); }
        for (int j = 0; j < 4; j++) U[j] = ns_ldq(up[j]);
        for (int j = 0; j < 4; j++) V[j] = ns_ldq(vp[j]);
        __m128i acc = _mm_setzero_si128();
        for (int j = 0; j < 4; j++) { d[j] = _mm_sub_ps(U[j], V[j]); sum[j] = _mm_add_ps(U[j], V[j]); NS_ACC(acc, d[j]); NS_ACC(acc, sum[j]); }
        const __m128 m0 = _mm_mul_ps(d[0], t.s), m1 = _mm_mul_ps(d[1], t.s), m2 = _mm_mul_ps(d[2], t.s), m3 = _mm_mul_ps(d[3], t.s);
        const __m128 v3 = _mm_fmadd_ps(d[3], t.c, m1), v2 = _mm_fmadd_ps(d[2], t.c, m0);
        const __m128 v0 = _mm_fmsub_ps(d[0], t.c, m2), v1 = _mm_fmsub_ps(d[1], t.c, m3);
        NS_ACC(acc, m0); NS_ACC(acc, m1); NS_ACC(acc, m2); NS_ACC(acc, m3);
        NS_ACC(acc, v0); NS_ACC(acc, v1); NS_ACC(acc, v2); NS_ACC(acc, v3);
        if (__builtin_expect(ns_bad(acc), 0)) break;
        for (int j = 0; j < 4; j++) ns_stq(up[j], NS_KEEP(sum[j]));
        ns_stq(vp[3], NS_KEEP(v3)); ns_stq(vp[2], NS_KEEP(v2)); ns_stq(vp[0], NS_KEEP(v0)); ns_stq(vp[1], NS_KEEP(v1));
        t.w = wn;
    }
    if (k) {
        g[17] = ns_splat_add(g[17], 64u * k); g[18] = ns_splat_add(g[18], 64u * k);
        g[19] = ns_du128(t.w);
        g[20] = ns_splat_add(g[20], 0u - k);
        NS_STAT(NS_EB_4158, fast_calls, 1); NS_STAT(NS_EB_4158, fast_iters, k);
    }
    if (k != n - 1) NS_STAT(NS_EB_4158, bail_range, 1);
}

/* ---------------------------------------------------------------------------------------------
 * 0x4008: inverse FFT, first stage: reads two streams (r15, r14, 2 lines a pass), writes two
 * (r12, r11, 4 lines a pass).
 *     d = LS[r15] - LS[r14]    (two lines)
 *     LS[r12] = LS[r15] + LS[r14] (2 lines);  LS[r12+0x20], LS[r12+0x30] = r19 (a constant register)
 *     LS[r11], +0x10 = d*c     LS[r11+0x20], +0x30 = d*s
 * twiddle w = r13, alpha = r21, beta = r18, mask = r17; splat control r20; count r16.
 * Carried: r11 r12 r13 r14 r15 r16.
 * ------------------------------------------------------------------------------------------- */
static void ns_fast_00004008(spu_context* ctx)
{
    u128* const g = ctx->gpr;
    uint8_t* const ls = ctx->ls;
    const uint32_t n = g[16]._u32[0];
    ns_tw t;
    if (n < 2 || n > 0x4000 || !CTL_SPLAT0(g[20]) || !ns_tw_init(&t, g[13], g[21], g[18], g[17])) {
        NS_STAT(NS_EB_4008, bail_precond, 1); return;
    }
    const uint32_t p15 = g[15]._u32[0], p14 = g[14]._u32[0], p12 = g[12]._u32[0], p11 = g[11]._u32[0];
    const __m128 fill = ns_reg(g[19]);
    uint32_t k = 0;
    for (; k < n - 1; k++) {
        __m128d wn;
        if (__builtin_expect(!ns_tw_step(&t, &wn), 0)) break;
        const uint32_t i = 32u * k, o = 64u * k;
        const __m128 E0 = ns_ldq(ns_line(ls, p15 + i)), E1 = ns_ldq(ns_line(ls, p15 + i + 16));
        const __m128 G0 = ns_ldq(ns_line(ls, p14 + i)), G1 = ns_ldq(ns_line(ls, p14 + i + 16));
        const __m128 d0 = _mm_sub_ps(E0, G0), d1 = _mm_sub_ps(E1, G1), s0 = _mm_add_ps(E0, G0), s1 = _mm_add_ps(E1, G1);
        const __m128 c0 = _mm_mul_ps(d0, t.c), c1 = _mm_mul_ps(d1, t.c), z1 = _mm_mul_ps(d1, t.s), z0 = _mm_mul_ps(d0, t.s);
        __m128i acc = ns_mag(d0);
        NS_ACC(acc, d1); NS_ACC(acc, s0); NS_ACC(acc, s1); NS_ACC(acc, c0); NS_ACC(acc, c1); NS_ACC(acc, z0); NS_ACC(acc, z1);
        if (__builtin_expect(ns_bad(acc), 0)) break;
        ns_stq(ns_line(ls, p12 + o + 0x20), fill); ns_stq(ns_line(ls, p12 + o + 0x30), fill);
        ns_stq(ns_line(ls, p12 + o), NS_KEEP(s0)); ns_stq(ns_line(ls, p12 + o + 0x10), NS_KEEP(s1));
        ns_stq(ns_line(ls, p11 + o), NS_KEEP(c0)); ns_stq(ns_line(ls, p11 + o + 0x10), NS_KEEP(c1));
        ns_stq(ns_line(ls, p11 + o + 0x30), NS_KEEP(z1)); ns_stq(ns_line(ls, p11 + o + 0x20), NS_KEEP(z0));
        t.w = wn;
    }
    if (k) {
        g[11] = ns_splat_add(g[11], 64u * k); g[12] = ns_splat_add(g[12], 64u * k);
        g[14] = ns_splat_add(g[14], 32u * k); g[15] = ns_splat_add(g[15], 32u * k);
        g[13] = ns_du128(t.w);
        g[16] = ns_splat_add(g[16], 0u - k);
        NS_STAT(NS_EB_4008, fast_calls, 1); NS_STAT(NS_EB_4008, fast_iters, k);
    }
    if (k != n - 1) NS_STAT(NS_EB_4008, bail_range, 1);
}

/* ---------------------------------------------------------------------------------------------
 * 0x3D80: forward FFT, last stage (no twiddle): pairs of 2 lines, in place.
 *     LS[r9+0x20] = LS[r9] - LS[r9+0x20]   LS[r9+0x30] = LS[r9+0x10] - LS[r9+0x30]
 *     LS[r9]      = LS[r9] + LS[r9+0x20]   LS[r9+0x10] = LS[r9+0x10] + LS[r9+0x30]
 * r9 += 64; r10 += 2 while r81 > r10 (unsigned).  Carried: r9 r10.
 * ------------------------------------------------------------------------------------------- */
static void ns_fast_00003D80(spu_context* ctx)
{
    u128* const g = ctx->gpr;
    uint8_t* const ls = ctx->ls;
    const uint32_t i0 = g[10]._u32[0], lim = g[81]._u32[0];
    /* passes: the loop adds 2 and goes on while lim > i (unsigned) */
    if (lim > 0x8000 || i0 >= lim) { NS_STAT(NS_EB_3D80, bail_precond, 1); return; }
    const uint32_t n = (lim - i0 + 1) / 2;
    if (n < 2) { NS_STAT(NS_EB_3D80, bail_precond, 1); return; }
    const uint32_t p = g[9]._u32[0];
    uint32_t k = 0;
    for (; k < n - 1; k++) {
        uint8_t* const q0 = ns_line(ls, p + 64u * k), * const q1 = ns_line(ls, p + 64u * k + 0x10);
        uint8_t* const q2 = ns_line(ls, p + 64u * k + 0x20), * const q3 = ns_line(ls, p + 64u * k + 0x30);
        const __m128 a = ns_ldq(q0), b = ns_ldq(q1), c = ns_ldq(q2), d = ns_ldq(q3);
        const __m128 d0 = _mm_sub_ps(a, c), d1 = _mm_sub_ps(b, d), s0 = _mm_add_ps(a, c), s1 = _mm_add_ps(b, d);
        __m128i acc = ns_mag(d0);
        NS_ACC(acc, d1); NS_ACC(acc, s0); NS_ACC(acc, s1);
        if (__builtin_expect(ns_bad(acc), 0)) break;
        ns_stq(q2, NS_KEEP(d0)); ns_stq(q3, NS_KEEP(d1)); ns_stq(q0, NS_KEEP(s0)); ns_stq(q1, NS_KEEP(s1));
    }
    if (k) {
        g[9] = ns_splat_add(g[9], 64u * k);
        g[10] = ns_splat_add(g[10], 2u * k);
        NS_STAT(NS_EB_3D80, fast_calls, 1); NS_STAT(NS_EB_3D80, fast_iters, k);
    }
    if (k != n - 1) NS_STAT(NS_EB_3D80, bail_range, 1);
}

/* ---------------------------------------------------------------------------------------------
 * 0x43C0: inverse FFT, the final scale by 1/N: 4 lines a pass, in place, LS[r6 ..] *= r7 (fm),
 * r8 passes.  Carried: r6 r8.
 * ------------------------------------------------------------------------------------------- */
static void ns_fast_000043C0(spu_context* ctx)
{
    u128* const g = ctx->gpr;
    uint8_t* const ls = ctx->ls;
    const uint32_t n = g[8]._u32[0];
    if (n < 2 || n > 0x4000) { NS_STAT(NS_EB_43C0, bail_precond, 1); return; }
    const uint32_t p = g[6]._u32[0];
    const __m128 sc = ns_reg(g[7]);
    uint32_t k = 0;
    for (; k < n - 1; k++) {
        uint8_t* const q0 = ns_line(ls, p + 64u * k), * const q1 = ns_line(ls, p + 64u * k + 0x10);
        uint8_t* const q2 = ns_line(ls, p + 64u * k + 0x20), * const q3 = ns_line(ls, p + 64u * k + 0x30);
        const __m128 t1 = _mm_mul_ps(ns_ldq(q1), sc), t0 = _mm_mul_ps(ns_ldq(q0), sc);
        const __m128 t3 = _mm_mul_ps(ns_ldq(q3), sc), t2 = _mm_mul_ps(ns_ldq(q2), sc);
        __m128i acc = ns_mag(t0);
        NS_ACC(acc, t1); NS_ACC(acc, t2); NS_ACC(acc, t3);
        if (__builtin_expect(ns_bad(acc), 0)) break;
        ns_stq(q1, NS_KEEP(t1)); ns_stq(q0, NS_KEEP(t0)); ns_stq(q3, NS_KEEP(t3)); ns_stq(q2, NS_KEEP(t2));
    }
    if (k) {
        g[6] = ns_splat_add(g[6], 64u * k);
        g[8] = ns_splat_add(g[8], 0u - k);
        NS_STAT(NS_EB_43C0, fast_calls, 1); NS_STAT(NS_EB_43C0, fast_iters, k);
    }
    if (k != n - 1) NS_STAT(NS_EB_43C0, bail_range, 1);
}


/* GT5P_NATIVE_EQ: 1 (default) native fast blocks, 0 lifted only. Read once. */
static int ns_mode(void)
{
    static volatile int mode = -1;   /* benign race: every thread reads the same value */
    if (mode < 0) { const char* e = getenv("GT5P_NATIVE_EQ"); mode = (e && *e == '0') ? 0 : 1; }
    return mode;
}

#define NS_ENTRY(A)                                                                    \
    void ORIG_FN(A)(spu_context* ctx);                                                 \
    void NATIVE_FN(A)(spu_context* ctx)                                                \
    {                                                                                  \
        if (__builtin_expect(ns_mode(), 1)) ns_fast_##A(ctx);                          \
        __attribute__((musttail)) return ORIG_FN(A)(ctx);                              \
    }
NS_ENTRY(00003C6C)
NS_ENTRY(00003ADC)
NS_ENTRY(00004158)
NS_ENTRY(00004008)
NS_ENTRY(00003D80)
NS_ENTRY(000043C0)
