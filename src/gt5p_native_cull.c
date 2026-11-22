/*
 * gt5p_native_cull.c -- the triangle loop of the car render job's cull as native code.
 *
 * Body b0196FF40 (workload 4: the car list's and the race's car render pass) culls each batch's
 * triangles at 0xAB00: a vertex stage leaves per vertex screen x, y and near / far flags in its
 * scratch (r8) and the batch's constants in registers; from 0xAF88 a software-pipelined loop
 * (0xAFC0..0xB204, 4 triangles a pass) decides every triangle and packs the kept ones in place
 * at the front of the list (r3); 0xB4E0 then restores the caller's registers and writes the
 * kept count. At Rome with the field in view the loop's functions are about a fifth of the
 * workload's time (progress-gt5.md, 2026-11-22).
 *
 * gt5_cull_af88 (a jump hook at 0xAF88, scripts/spu_native_patch.py) decides 8 triangles a
 * step in AVX2 and makes the loop's stores as the lifted loop makes them, sets the two
 * registers the loop leaves for 0xB4E0 (r13 the write position, r40 the read counter) and
 * returns 1: the lifted code goes on at 0xB4E0. It returns 0 (the lifted loop runs) for an
 * interrupt pending, a sample pattern 1 or 5 (not verified), a value with exponent 255, an
 * index past the vertices, counters of another shape, or a CPU without AVX2/FMA.
 *
 * The decision logic and the store model are gt5-recomp's (MIT License, Copyright (c) 2026
 * MormonFTP-hub, GT5/src/native/native_w018CAD00.c, nx_decide / nx_pack, 2026-11-20; their job
 * w018CAD00 is byte-identical to b0196FF40). Changed for this runtime: its SPU float runs in the
 * thread's own MXCSR mode (no round-toward-zero requirement: the same host operations give the
 * same bits in any mode), and fma/fnms are computed as the lifted copy does them, fused in an
 * FMA copy, through doubles otherwise (`fused`, from the hook).
 *
 * GT5P_NATIVE_CULL=0: the lifted loop always. Test: scripts/test/native_cull.py (captured states:
 * local store and every register but the volatile r2-r79 the same as the lifted run).
 *
 * The optional B210 -> B4E0 variant uses the guest's 512-byte coverage table:
 * quantize bounds in eighths, clamp the Y span for the table, index by Y/X
 * phases, then compare its threshold against X span. Its pipeline starts
 * with a different carried quadword and two fill batches. Only measured
 * pattern 5 is enabled; aliases and unknown values remain lifted.
 * GT5P_NATIVE_CULL_TABLE=1 opts in (default off). --table-only --expand in
 * native_cull.py compares against the existing native cull, requires this
 * path to be exercised, and varies batch counts and facing flags.
 */
#include "spu_context.h"
#include <immintrin.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define NC_LSM (SPU_LS_MASK & ~0u)
static inline uint32_t nc_be32(const uint8_t* ls, uint32_t a) { uint32_t v; memcpy(&v, ls + (a & SPU_LS_MASK), 4); return __builtin_bswap32(v); }

/* ---- the sample pattern of the batch 0xAB00 started (a plain hook there) ---- */
#if defined(_MSC_VER) && !defined(__clang__)
#define NC_TLS __declspec(thread)
#else
#define NC_TLS _Thread_local
#endif
static NC_TLS const spu_context* t_nc_ctx;
static NC_TLS uint32_t t_nc_pat;
void gt5p_cull_ab00(spu_context* ctx)
{
    t_nc_ctx = ctx;
    t_nc_pat = ctx->ls[ctx->gpr[5]._u32[0] & SPU_LS_MASK & ~15u];
}

/* GT5P_NATIVE_CULL (read once) and the CPU check: AVX2, FMA and the OS saving YMM state */
#include <intrin.h>
static int nc_cpu_ok(void)
{
    int r[4];
    __cpuid(r, 1);
    const int fma = (r[2] >> 12) & 1, osxsave = (r[2] >> 27) & 1, avx = (r[2] >> 28) & 1;
    if (!(fma && osxsave && avx) || (_xgetbv(0) & 6) != 6) return 0;
    __cpuidex(r, 7, 0);
    return (r[1] >> 5) & 1;   /* AVX2 */
}
static int g_nc_on = -1;
static __attribute__((noinline)) int nc_init(void)
{
    const char* e = getenv("GT5P_NATIVE_CULL");
    int on = !(e && e[0] == '0');
    if (on && !nc_cpu_ok()) on = 0;
#ifdef NC_FORCE_OFF   /* scripts/test/native_cull.py: the lifted reference build */
    on = 0;
#endif
    g_nc_on = on;
    return on;
}

typedef struct {
    float bias, k1, k2, xlim, ylim;
    uint32_t flip;          /* r36: 0, or the sign bit */
    int two_sided;          /* r10 == 16 */
    uint32_t cover_mask;    /* r58 */
    uint32_t near_off;      /* r50: ~0 = no near-plane test */
} nc_consts;

#pragma clang attribute push (__attribute__((target("avx2,fma"))), apply_to = function)

/* SPU cflts(x, 172): x * 2 truncated toward zero, saturated to int32 */
static inline __m256i nc_cflts2(__m256 x)
{
    const __m256 t = _mm256_add_ps(x, x);
    __m256i r = _mm256_cvttps_epi32(t);
    const __m256 pos_over = _mm256_cmp_ps(t, _mm256_set1_ps(2147483648.0f), _CMP_GE_OQ);
    return _mm256_blendv_epi8(r, _mm256_set1_epi32(0x7FFFFFFF), _mm256_castps_si256(pos_over));
}
static inline __m256i nc_cflts8(__m256 x)
{
    const __m256 t = _mm256_mul_ps(x, _mm256_set1_ps(8.0f));
    const __m256i r = _mm256_cvttps_epi32(t);
    return _mm256_blendv_epi8(r, _mm256_set1_epi32(0x7FFFFFFF),
        _mm256_castps_si256(_mm256_cmp_ps(t, _mm256_set1_ps(2147483648.0f), _CMP_GE_OQ)));
}
/* a * b + c as the lifted copy computes it: fused, or (float)((double)a * b + c) */
static inline __m256 nc_fma(__m256 a, __m256 b, __m256 c, int fused)
{
    if (fused) return _mm256_fmadd_ps(a, b, c);
    const __m256d lo = _mm256_add_pd(_mm256_mul_pd(_mm256_cvtps_pd(_mm256_castps256_ps128(a)), _mm256_cvtps_pd(_mm256_castps256_ps128(b))),
                                     _mm256_cvtps_pd(_mm256_castps256_ps128(c)));
    const __m256d hi = _mm256_add_pd(_mm256_mul_pd(_mm256_cvtps_pd(_mm256_extractf128_ps(a, 1)), _mm256_cvtps_pd(_mm256_extractf128_ps(b, 1))),
                                     _mm256_cvtps_pd(_mm256_extractf128_ps(c, 1)));
    return _mm256_set_m128(_mm256_cvtpd_ps(hi), _mm256_cvtpd_ps(lo));
}
/* c - a * b: spu_fnms(a, b, c) = fmaf(a, -b, c) */
static inline __m256 nc_fnms(__m256 a, __m256 b, __m256 c, int fused)
{
    if (fused) return _mm256_fnmadd_ps(a, b, c);
    const __m256 nb = _mm256_xor_ps(b, _mm256_set1_ps(-0.0f));
    return nc_fma(a, nb, c, 0);
}
/* the 16-byte host-order lines (x y near far) of 8 vertices -> x[8], y[8], near[8], far[8] */
static inline void nc_role(const uint32_t* soa, const int32_t* idx, __m256* x, __m256* y, __m256i* nn, __m256i* ff)
{
    const __m128i* L = (const __m128i*)soa;
    const __m256i r0 = _mm256_setr_m128i(_mm_load_si128(L + idx[0]), _mm_load_si128(L + idx[4]));
    const __m256i r1 = _mm256_setr_m128i(_mm_load_si128(L + idx[1]), _mm_load_si128(L + idx[5]));
    const __m256i r2 = _mm256_setr_m128i(_mm_load_si128(L + idx[2]), _mm_load_si128(L + idx[6]));
    const __m256i r3 = _mm256_setr_m128i(_mm_load_si128(L + idx[3]), _mm_load_si128(L + idx[7]));
    const __m256i t0 = _mm256_unpacklo_epi32(r0, r1), t1 = _mm256_unpackhi_epi32(r0, r1);
    const __m256i t2 = _mm256_unpacklo_epi32(r2, r3), t3 = _mm256_unpackhi_epi32(r2, r3);
    *x = _mm256_castsi256_ps(_mm256_unpacklo_epi64(t0, t2)); *y = _mm256_castsi256_ps(_mm256_unpackhi_epi64(t0, t2));
    *nn = _mm256_unpacklo_epi64(t1, t3); *ff = _mm256_unpackhi_epi64(t1, t3);
}
/* keep: bit (t & 7) of byte t >> 3 = triangle t kept. The number kept, or -1 for a value
 * outside what the x86 operations reproduce (the lifted loop then runs). */
static int nc_decide(const uint8_t* ls, uint32_t list, uint32_t scratch, uint32_t ntri, uint32_t nvert,
                     const nc_consts* k, uint8_t* keep, int fused, const uint8_t* coverage)
{
    const __m256 bias = _mm256_set1_ps(k->bias), k1 = _mm256_set1_ps(k->k1), k2 = _mm256_set1_ps(k->k2);
    const __m256 xlim = _mm256_set1_ps(k->xlim), ylim = _mm256_set1_ps(k->ylim);
    const __m256 absm = _mm256_castsi256_ps(_mm256_set1_epi32(0x7FFFFFFF));
    const __m256i flip = _mm256_set1_epi32((int)k->flip);
    const __m256i expmask = _mm256_set1_epi32(0x7F800000);
    __m256i bad = _mm256_setzero_si256();
    int kept = 0;
    static NC_TLS uint32_t* soa; static NC_TLS uint32_t cap;
    const uint32_t nv4 = (nvert + 3u) & ~3u;
    if (nv4 > 4096u || scratch + 16u * nv4 > SPU_LS_SIZE || (scratch & 15u)) return -1;
    if ((list & 7u) || list + 8u * ((ntri + 7u) & ~7u) > SPU_LS_SIZE) return -1;
    if (4u * nv4 > cap) { _mm_free(soa); cap = 4u * nv4 + 64u; soa = (uint32_t*)_mm_malloc(4u * cap, 32); if (!soa) { cap = 0; return -1; } }
    const __m256i bsw = _mm256_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12, 3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
    for (uint32_t v = 0; v < nv4; v += 2)
        _mm256_store_si256((__m256i*)(soa + 4u * v), _mm256_shuffle_epi8(_mm256_loadu_si256((const __m256i*)(ls + scratch + 16u * v)), bsw));
    for (uint32_t t0 = 0; t0 < ntri; t0 += 8) {
        int32_t i0v[8], i1v[8], i2v[8];
        const uint32_t n = ntri - t0 < 8 ? ntri - t0 : 8;
        {
            const __m256i hsw = _mm256_setr_epi8(1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14, 1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14);
            const uint8_t* e = ls + list + 8u * t0;
            const __m256i v0 = _mm256_shuffle_epi8(_mm256_loadu_si256((const __m256i*)e), hsw);
            const __m256i v1 = _mm256_shuffle_epi8(_mm256_loadu_si256((const __m256i*)(e + 32)), hsw);
            const __m256i ev = _mm256_setr_epi32(0, 2, 4, 6, 1, 3, 5, 7);
            const __m256i p0 = _mm256_permutevar8x32_epi32(v0, ev), p1 = _mm256_permutevar8x32_epi32(v1, ev);
            const __m256i E = _mm256_permute2x128_si256(p0, p1, 0x20), O = _mm256_permute2x128_si256(p0, p1, 0x31);
            const __m256i lo = _mm256_set1_epi32(0xFFFF);
            __m256i I0 = _mm256_and_si256(E, lo), I1 = _mm256_srli_epi32(E, 16), I2 = _mm256_and_si256(O, lo);
            if (n < 8) {
                const __m256i lane = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7), valid = _mm256_cmpgt_epi32(_mm256_set1_epi32((int)n), lane);
                I0 = _mm256_blendv_epi8(_mm256_permutevar8x32_epi32(I0, _mm256_setzero_si256()), I0, valid);
                I1 = _mm256_blendv_epi8(_mm256_permutevar8x32_epi32(I1, _mm256_setzero_si256()), I1, valid);
                I2 = _mm256_blendv_epi8(_mm256_permutevar8x32_epi32(I2, _mm256_setzero_si256()), I2, valid);
            }
            const __m256i lim = _mm256_set1_epi32((int)nv4 - 1);
            const __m256i over = _mm256_or_si256(_mm256_cmpgt_epi32(I0, lim), _mm256_or_si256(_mm256_cmpgt_epi32(I1, lim), _mm256_cmpgt_epi32(I2, lim)));
            if (!_mm256_testz_si256(over, over)) return -1;
            _mm256_storeu_si256((__m256i*)i0v, I0); _mm256_storeu_si256((__m256i*)i1v, I1); _mm256_storeu_si256((__m256i*)i2v, I2);
        }
        __m256 Ax, Ay, Bx, By, Cx, Cy; __m256i An, Bn, Cn, Af, Bf, Cf;
        nc_role(soa, i0v, &Ax, &Ay, &An, &Af);
        nc_role(soa, i1v, &Bx, &By, &Bn, &Bf);
        nc_role(soa, i2v, &Cx, &Cy, &Cn, &Cf);
        bad = _mm256_or_si256(bad, _mm256_cmpeq_epi32(_mm256_and_si256(_mm256_castps_si256(Ax), expmask), expmask));
        bad = _mm256_or_si256(bad, _mm256_cmpeq_epi32(_mm256_and_si256(_mm256_castps_si256(Ay), expmask), expmask));
        bad = _mm256_or_si256(bad, _mm256_cmpeq_epi32(_mm256_and_si256(_mm256_castps_si256(Bx), expmask), expmask));
        bad = _mm256_or_si256(bad, _mm256_cmpeq_epi32(_mm256_and_si256(_mm256_castps_si256(By), expmask), expmask));
        bad = _mm256_or_si256(bad, _mm256_cmpeq_epi32(_mm256_and_si256(_mm256_castps_si256(Cx), expmask), expmask));
        bad = _mm256_or_si256(bad, _mm256_cmpeq_epi32(_mm256_and_si256(_mm256_castps_si256(Cy), expmask), expmask));
        const __m256 dxb = _mm256_sub_ps(Bx, Ax), dxc = _mm256_sub_ps(Cx, Ax), dyb = _mm256_sub_ps(By, Ay), dyc = _mm256_sub_ps(Cy, Ay);
        const __m256 s = _mm256_add_ps(_mm256_add_ps(_mm256_and_ps(dxb, absm), _mm256_and_ps(dxc, absm)),
                                       _mm256_add_ps(_mm256_and_ps(dyb, absm), _mm256_and_ps(dyc, absm)));
        const __m256 eps = nc_fma(s, k1, k2, fused);
        const __m256 tt = nc_fnms(dyc, dxb, eps, fused);
        const __m256 uu = nc_fma(dxc, dyb, tt, fused);
        bad = _mm256_or_si256(bad, _mm256_cmpeq_epi32(_mm256_and_si256(_mm256_castps_si256(s), expmask), expmask));
        bad = _mm256_or_si256(bad, _mm256_cmpeq_epi32(_mm256_and_si256(_mm256_castps_si256(uu), expmask), expmask));
        const __m256i degenerate = _mm256_cmpeq_epi32(_mm256_castps_si256(uu), _mm256_castps_si256(eps));
        const __m256 gx = _mm256_cmp_ps(Ax, Bx, _CMP_GT_OQ), gy = _mm256_cmp_ps(Ay, By, _CMP_GT_OQ);
        const __m256 mxAB = _mm256_blendv_ps(Bx, Ax, gx), mnAB = _mm256_blendv_ps(Ax, Bx, gx);
        const __m256 myAB = _mm256_blendv_ps(By, Ay, gy), nyAB = _mm256_blendv_ps(Ay, By, gy);
        const __m256 xmax = _mm256_blendv_ps(mxAB, Cx, _mm256_cmp_ps(Cx, mxAB, _CMP_GT_OQ));
        const __m256 xmin = _mm256_blendv_ps(Cx, mnAB, _mm256_cmp_ps(Cx, mnAB, _CMP_GT_OQ));
        const __m256 ymax = _mm256_blendv_ps(myAB, Cy, _mm256_cmp_ps(Cy, myAB, _CMP_GT_OQ));
        const __m256 ymin = _mm256_blendv_ps(Cy, nyAB, _mm256_cmp_ps(Cy, nyAB, _CMP_GT_OQ));
        const __m256 xmaxb = _mm256_add_ps(xmax, bias), ymaxb = _mm256_add_ps(ymax, bias);
        const __m256i cmask = _mm256_set1_epi32((int)k->cover_mask);
        __m256i cover = _mm256_and_si256(_mm256_sub_epi32(nc_cflts2(xmin), _mm256_and_si256(nc_cflts2(xmaxb), cmask)),
                                               _mm256_sub_epi32(nc_cflts2(ymin), _mm256_and_si256(nc_cflts2(ymaxb), cmask)));
        if (coverage) {
            uint32_t ix[8], iy[8], dx[8], dy[8], hit[8];
            const __m256i X = nc_cflts8(xmin), Y = nc_cflts8(ymin);
            _mm256_storeu_si256((__m256i*)ix, X); _mm256_storeu_si256((__m256i*)iy, Y);
            _mm256_storeu_si256((__m256i*)dx, _mm256_sub_epi32(nc_cflts8(xmaxb), X));
            _mm256_storeu_si256((__m256i*)dy, _mm256_sub_epi32(nc_cflts8(ymaxb), Y));
            for (unsigned j = 0; j < 8; j++) {
                /* The guest indexes by Y span and Y/X phases, then compares
                 * the threshold with the X span. Its rotate stays in a line. */
                const uint32_t q = ((dy[j] > 7u ? 7u : dy[j]) << 6) + ((iy[j] & 7u) << 3) + (ix[j] & 7u);
                const unsigned threshold = coverage[(q & ~15u) | ((q + 3u) & 15u)];
                hit[j] = (int32_t)dx[j] >= (int32_t)threshold ? ~0u : 0u;
            }
            cover = _mm256_loadu_si256((const __m256i*)hit);
        }
        const __m256i top = _mm256_set1_epi32((int)0xFF000000u);
        const __m256i nA = _mm256_cmpeq_epi32(_mm256_and_si256(An, top), top), nB = _mm256_cmpeq_epi32(_mm256_and_si256(Bn, top), top);
        const __m256i nC = _mm256_cmpeq_epi32(_mm256_and_si256(Cn, top), top);
        const __m256i zA = _mm256_cmpeq_epi32(_mm256_and_si256(An, top), _mm256_setzero_si256());
        const __m256i zB = _mm256_cmpeq_epi32(_mm256_and_si256(Bn, top), _mm256_setzero_si256());
        const __m256i zC = _mm256_cmpeq_epi32(_mm256_and_si256(Cn, top), _mm256_setzero_si256());
        const __m256i noff = _mm256_set1_epi32((int)k->near_off);
        const __m256i allnear = _mm256_andnot_si256(noff, _mm256_and_si256(nA, _mm256_and_si256(nB, nC)));
        const __m256i nonenear = _mm256_or_si256(noff, _mm256_and_si256(zA, _mm256_and_si256(zB, zC)));
        const __m256i facing_on = _mm256_or_si256(allnear, nonenear);
        const __m256i allfar = _mm256_and_si256(Af, _mm256_and_si256(Bf, Cf));
        __m256i facing = _mm256_xor_si256(_mm256_castps_si256(uu), flip);
        if (k->two_sided) facing = _mm256_setzero_si256();
        facing = _mm256_and_si256(facing, facing_on);
        __m256i rej = _mm256_or_si256(degenerate, _mm256_andnot_si256(cover, _mm256_set1_epi32(-1)));
        rej = _mm256_or_si256(rej, facing);
        rej = _mm256_or_si256(rej, allnear);
        rej = _mm256_or_si256(rej, allfar);
        rej = _mm256_or_si256(rej, _mm256_castps_si256(xmaxb));
        rej = _mm256_or_si256(rej, _mm256_castps_si256(ymaxb));
        rej = _mm256_or_si256(rej, _mm256_castps_si256(_mm256_cmp_ps(xmin, xlim, _CMP_GT_OQ)));
        rej = _mm256_or_si256(rej, _mm256_castps_si256(_mm256_cmp_ps(ymin, ylim, _CMP_GT_OQ)));
        const uint32_t km = (~(uint32_t)_mm256_movemask_ps(_mm256_castsi256_ps(rej))) & ((1u << n) - 1u);
        keep[t0 >> 3] = (uint8_t)km;
        kept += __builtin_popcount(km);
    }
    if (_mm256_movemask_ps(_mm256_castsi256_ps(bad))) return -1;
    return kept;
}

#pragma clang attribute pop

/* the loop's stores. 0 when its counters are not the shape this model knows (nothing written). */
static int nc_pack(spu_context* ctx, const uint8_t* keep, uint32_t ntri, int coverage)
{
    uint8_t* const ls = ctx->ls;
    const uint32_t list = ctx->gpr[3]._u32[0];
    const u128 R13 = ctx->gpr[13], R40 = ctx->gpr[40];
    const int32_t r40e = (int32_t)R40._u32[0];
    if (r40e > 0 || (r40e & 31) || (uint32_t)(-r40e) / 32u != (ntri + 3u) / 4u) return 0;
    const uint32_t P = (uint32_t)(-r40e) / 32u + 2u;                      /* passes: (quads) + 2 of fill */
    for (uint32_t j = 0; j < 4; j++)                                       /* the last pass's lane mask is the count's */
        if ((ctx->gpr[43]._u32[j] != 0) != (4u * ((ntri + 3u) / 4u - 1u) + j >= ntri)) return 0;
    uint8_t Q[16], fill[2][4][8];
    for (int i = 0; i < 4; i++) { const uint32_t w = __builtin_bswap32(ctx->gpr[coverage ? 87 : 84]._u32[i]); memcpy(Q + 4 * i, &w, 4); }
    const int fr[4] = { 45, coverage ? 53 : 32, 37, coverage ? 32 : 57 };
    for (int f = 0; f < 4; f++) {
        uint8_t b[16];
        for (int i = 0; i < 4; i++) { const uint32_t w = __builtin_bswap32(ctx->gpr[fr[f]]._u32[i]); memcpy(b + 4 * i, &w, 4); }
        memcpy(fill[f >> 1][2 * (f & 1)], b, 8); memcpy(fill[f >> 1][2 * (f & 1) + 1], b + 8, 8);
    }
    uint32_t pos = R13._u32[0], kept = 0;
    uint64_t qlo, qhi, plo = 0, phi = 0;                                   /* the quadword carried; the last one stored */
    memcpy(&qlo, Q, 8); memcpy(&qhi, Q + 8, 8);
    uint32_t cur = 0xFFFFFFFFu;                                            /* where it was stored: only its last store there stays */
    uint8_t quad[3][32];                                                   /* pass p loads quad p, stores quad p - 2's lanes */
    for (uint32_t p = 0; p < P; p++) {
        if (p + 2u < P) {
            const uint32_t a = (list + 32u * p) & SPU_LS_MASK;
            if (a + 32u <= SPU_LS_SIZE) memcpy(quad[p % 3u], ls + a, 32);
            else for (uint32_t i = 0; i < 32u; i++) quad[p % 3u][i] = ls[(a + i) & SPU_LS_MASK];
        }
        for (uint32_t j = 0; j < 4; j++) {
            uint64_t d;
            uint32_t kk = 0;
            if (p < 2) memcpy(&d, fill[p][j], 8);
            else {
                const uint32_t t = 4u * (p - 2u) + j;
                memcpy(&d, quad[(p - 2u) % 3u] + 8u * j, 8);
                kk = t < ntri ? (keep[t >> 3] >> (t & 7)) & 1u : 0u;
            }
            if (pos & 8u) qhi = d;
            else { qhi = qlo; qlo = d; }
            const uint32_t addr = (list + pos) & SPU_LS_MASK & ~15u;     /* stqx: the quadword at the list + the position */
            if (addr != cur) {
                if (cur != 0xFFFFFFFFu) { memcpy(ls + cur, &plo, 8); memcpy(ls + cur + 8, &phi, 8); }
                cur = addr;
            }
            plo = qlo; phi = qhi;
            if (kk) { pos += 8u; kept++; }
        }
    }
    if (cur != 0xFFFFFFFFu) { memcpy(ls + cur, &plo, 8); memcpy(ls + cur + 8, &phi, 8); }
    for (int i = 0; i < 4; i++) { ctx->gpr[13]._u32[i] = R13._u32[i] + 8u * kept; ctx->gpr[40]._u32[i] = R40._u32[i] + 32u * P; }
    return 1;
}

/* The jump hook at 0xAF88: 1 = the loop is done (r13, r40 and the list written), go on at 0xB4E0;
 * 0 = nothing changed, the lifted loop runs. */
static int nc_try_cull(spu_context* ctx, int fused, int coverage)
{
    static NC_TLS uint8_t keep[0x8000 / 8 + 8];
    int on = g_nc_on;
    if (__builtin_expect(on < 0, 0)) on = nc_init();
    if (!on) return 0;
    if (ctx->int_enable && (ctx->event_status & ctx->event_mask)) return 0;
    if (t_nc_ctx != ctx || (!coverage && (t_nc_pat == 1u || t_nc_pat == 5u))) return 0;
    if (coverage && t_nc_pat != 5u) return 0; /* captured/verified table pattern */
    const uint8_t* ls = ctx->ls;
    const uint32_t table = ctx->gpr[28]._u32[0];
    if (coverage && ((table & 15u) || table > SPU_LS_SIZE - 512u)) return 0;
    const uint32_t line = ctx->gpr[4]._u32[0] & SPU_LS_MASK & ~15u;
    const uint32_t ntri = nc_be32(ls, line), nvert = nc_be32(ls, line + 4);
    if (!ntri || ntri > 0x8000u) return 0;
    if (coverage) {
        const uint32_t list = ctx->gpr[3]._u32[0], scratch = ctx->gpr[8]._u32[0];
        const uint32_t list_end = list + 8u * ((ntri + 7u) & ~7u);
        const uint32_t scratch_end = scratch + 16u * ((nvert + 3u) & ~3u);
        /* Deciding first is valid only when packing cannot overwrite its
         * vertex or coverage inputs. Unusual aliases stay lifted. */
        if (list_end < list || scratch_end < scratch ||
            (list < table + 512u && table < list_end) ||
            (list < scratch_end && scratch < list_end)) return 0;
    }
    nc_consts k;
    uint32_t u;
    u = ctx->gpr[5]._u32[0]; memcpy(&k.bias, &u, 4); u = ctx->gpr[6]._u32[0]; memcpy(&k.k1, &u, 4);
    u = ctx->gpr[34]._u32[0]; memcpy(&k.k2, &u, 4); u = ctx->gpr[55]._u32[0]; memcpy(&k.xlim, &u, 4);
    u = ctx->gpr[14]._u32[0]; memcpy(&k.ylim, &u, 4);
    k.flip = ctx->gpr[36]._u32[0]; k.two_sided = ctx->gpr[10]._u32[0] == 16u;
    k.cover_mask = ctx->gpr[58]._u32[0]; k.near_off = ctx->gpr[50]._u32[0];
    if (coverage) {
        const unsigned constants[] = {5, 6, 34, 55, 14};
        for (unsigned i = 0; i < sizeof constants / sizeof *constants; i++)
            if ((ctx->gpr[constants[i]]._u32[0] & 0x7F800000u) == 0x7F800000u) return 0;
    }
    const int kn = nc_decide(ls, ctx->gpr[3]._u32[0], ctx->gpr[8]._u32[0], ntri, nvert, &k, keep, fused, coverage ? ls + table : NULL);
    if (kn < 0) return 0;
    return nc_pack(ctx, keep, ntri, coverage);
}
int gt5p_cull_af88(spu_context* ctx, int fused) { return nc_try_cull(ctx, fused, 0); }
#ifdef NC_TABLE_TEST
static unsigned s_nc_table_hits;
#endif
int gt5p_cull_b210(spu_context* ctx, int fused)
{
    static int mode = -1;
    int enabled = __atomic_load_n(&mode, __ATOMIC_ACQUIRE);
    if (enabled < 0) {
        const char* e = getenv("GT5P_NATIVE_CULL_TABLE"); enabled = !(e && *e == '0');
        __atomic_store_n(&mode, enabled, __ATOMIC_RELEASE);
    }
#ifdef NC_TABLE_FORCE_OFF
    enabled = 0;
#endif
    const int taken = enabled ? nc_try_cull(ctx, fused, 1) : 0;
#ifdef NC_TABLE_TEST
    s_nc_table_hits += taken != 0;
#endif
    return taken;
}
