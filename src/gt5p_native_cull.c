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
#include "spu_helpers.h"
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
/* a * b - c: spu_fms(a, b, c) = fmaf(a, b, -c) */
static inline __m256 nc_fms(__m256 a, __m256 b, __m256 c, int fused)
{
    if (fused) return _mm256_fmsub_ps(a, b, c);
    const __m256 nc = _mm256_xor_ps(c, _mm256_set1_ps(-0.0f));
    return nc_fma(a, b, nc, 0);
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

/* Native AVX2 vertex transform for b0196FF40 0xAD30..0xAEB4.
 * Transforms nv4 (multiple of 4) 16-byte vertices at ls[src_base] into
 * {screen_x, screen_y, near, far} at ls[dst_base], bit-exact with 0xAD30. */
static int nc_vtx_transform(uint8_t* ls, uint32_t src_base, uint32_t dst_base, uint32_t nv4,
                            const spu_context* ctx, int fused)
{
    const __m256i bsw = _mm256_setr_epi8(
        3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12,
        3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
    const __m256 wx = _mm256_set1_ps(ctx->gpr[31]._f32[0]), wy = _mm256_set1_ps(ctx->gpr[32]._f32[0]);
    const __m256 wz = _mm256_set1_ps(ctx->gpr[34]._f32[0]), ww = _mm256_set1_ps(ctx->gpr[35]._f32[0]);
    const __m256 yx = _mm256_set1_ps(ctx->gpr[39]._f32[0]), yy = _mm256_set1_ps(ctx->gpr[41]._f32[0]);
    const __m256 yz = _mm256_set1_ps(ctx->gpr[43]._f32[0]), yw = _mm256_set1_ps(ctx->gpr[45]._f32[0]);
    const __m256 xx = _mm256_set1_ps(ctx->gpr[47]._f32[0]), xy = _mm256_set1_ps(ctx->gpr[49]._f32[0]);
    const __m256 xz = _mm256_set1_ps(ctx->gpr[50]._f32[0]), xw = _mm256_set1_ps(ctx->gpr[51]._f32[0]);
    const __m256 zx = _mm256_set1_ps(ctx->gpr[52]._f32[0]), zy = _mm256_set1_ps(ctx->gpr[53]._f32[0]);
    const __m256 zz = _mm256_set1_ps(ctx->gpr[54]._f32[0]), zw = _mm256_set1_ps(ctx->gpr[55]._f32[0]);
    const __m256 offY = _mm256_set1_ps(ctx->gpr[28]._f32[0]), offX = _mm256_set1_ps(ctx->gpr[29]._f32[0]);
    const __m256 one = _mm256_set1_ps(1.0f), c_1_64 = _mm256_set1_ps(0.015625f);
    const __m256 sgn_m = _mm256_castsi256_ps(_mm256_set1_epi32((int)0x80000000u));
    const __m256 abs_m = _mm256_castsi256_ps(_mm256_set1_epi32(0x7FFFFFFF));
    const __m256i one_bits = _mm256_set1_epi32(0x3F800000);
    const __m256i c253 = _mm256_set1_epi32(253), c13 = _mm256_set1_epi32(13), c14 = _mm256_set1_epi32(14);
    const __m256i mFF = _mm256_set1_epi32(0xFF), m1F = _mm256_set1_epi32(0x1F);
    const __m256i m3FF = _mm256_set1_epi32(0x3FF), m7FFFF = _mm256_set1_epi32(0x7FFFF);
    const __m256i mBase = _mm256_set1_epi32((int)0xFFFFFC00u);
    const __m256 inv524k = _mm256_set1_ps(1.0f / 524288.0f);
    const __m256i zero_i = _mm256_setzero_si256();

    for (uint32_t v = 0; v < nv4; v += 8) {
        const int tail4 = (v + 4u == nv4);
        const uint8_t* sp = ls + src_base + 16u * v;
        const uint8_t* sp2 = tail4 ? sp : (sp + 64);
        /* Load 8 vertices (low 128-bit lane = v+0..v+3, high 128-bit lane = v+4..v+7). */
        const __m256i r0 = _mm256_shuffle_epi8(_mm256_setr_m128i(
            _mm_loadu_si128((const __m128i*)(sp + 0)), _mm_loadu_si128((const __m128i*)(sp2 + 0))), bsw);
        const __m256i r1 = _mm256_shuffle_epi8(_mm256_setr_m128i(
            _mm_loadu_si128((const __m128i*)(sp + 16)), _mm_loadu_si128((const __m128i*)(sp2 + 16))), bsw);
        const __m256i r2 = _mm256_shuffle_epi8(_mm256_setr_m128i(
            _mm_loadu_si128((const __m128i*)(sp + 32)), _mm_loadu_si128((const __m128i*)(sp2 + 32))), bsw);
        const __m256i r3 = _mm256_shuffle_epi8(_mm256_setr_m128i(
            _mm_loadu_si128((const __m128i*)(sp + 48)), _mm_loadu_si128((const __m128i*)(sp2 + 48))), bsw);

        /* Transpose AoS -> SoA (X, Y, Z). */
        const __m256i t0 = _mm256_unpacklo_epi32(r0, r1), t1 = _mm256_unpackhi_epi32(r0, r1);
        const __m256i t2 = _mm256_unpacklo_epi32(r2, r3), t3 = _mm256_unpackhi_epi32(r2, r3);
        const __m256 X = _mm256_castsi256_ps(_mm256_unpacklo_epi64(t0, t2));
        const __m256 Y = _mm256_castsi256_ps(_mm256_unpackhi_epi64(t0, t2));
        const __m256 Z = _mm256_castsi256_ps(_mm256_unpacklo_epi64(t1, t3));

        /* 4x3 matrix multiply (exact operand order of 0xAD30). */
        const __m256 W  = nc_fma(Z, wz, nc_fma(Y, wy, nc_fma(X, wx, ww, fused), fused), fused);
        const __m256 Zc = nc_fma(Z, zz, nc_fma(Y, zy, nc_fma(X, zx, zw, fused), fused), fused);
        const __m256 Xc = nc_fma(Z, xz, nc_fma(xy, Y, nc_fma(X, xx, xw, fused), fused), fused);
        const __m256 Yc = nc_fma(Z, yz, nc_fma(Y, yy, nc_fma(X, yx, yw, fused), fused), fused);

        /* abs(W), -abs(W), frest + fi + 1 Newton-Raphson refinement. */
        const __m256 absW    = _mm256_and_ps(W, abs_m);
        const __m256 negAbsW = _mm256_or_ps(W, sgn_m);
        const __m256i ai     = _mm256_castps_si256(absW);
        const __m256i exp    = _mm256_and_si256(_mm256_srli_epi32(ai, 23), mFF);
        const __m256i fidx   = _mm256_and_si256(_mm256_srli_epi32(ai, 18), m1F);
        const __m256i diff   = _mm256_sub_epi32(c253, exp);
        const __m256i rexp   = _mm256_slli_epi32(_mm256_and_si256(diff, _mm256_cmpgt_epi32(diff, zero_i)), 23);
        const __m256i frac   = _mm256_i32gather_epi32((const int*)spu_frest_fraction_lut, fidx, 4);
        const __m256i est    = _mm256_blendv_epi8(_mm256_or_si256(rexp, frac),
                                                  _mm256_set1_epi32(0x7FFFFFFF),
                                                  _mm256_cmpeq_epi32(exp, zero_i));
        const __m256i e_est  = _mm256_and_si256(_mm256_srli_epi32(est, 23), mFF);
        __m256 fi_val;
        if (__builtin_expect(_mm256_movemask_epi8(_mm256_cmpgt_epi32(c14, e_est)), 0)) {
            uint32_t a8[8], b8[8]; float f8[8];
            _mm256_storeu_si256((__m256i*)a8, ai);
            _mm256_storeu_si256((__m256i*)b8, est);
            for (int k = 0; k < 8; k++) {
                uint32_t rb = b8[k], ra = a8[k];
                uint32_t eb = (rb >> 23) & 0xFFu;
                uint32_t bb = rb & 0xFFFFFC00u;
                float b_f; memcpy(&b_f, &bb, 4);
                float st = spu__ldexpf((float)(rb & 0x3FFu), (int)eb - 127 - 13);
                if (b_f < 0.0f) st = -st;
                float yf = spu__ldexpf((float)(ra & 0x7FFFFu), -19);
                f8[k] = b_f - st * yf;
            }
            fi_val = _mm256_loadu_ps(f8);
        } else {
            const __m256 base    = _mm256_castsi256_ps(_mm256_and_si256(est, mBase));
            const __m256 scale   = _mm256_castsi256_ps(_mm256_slli_epi32(_mm256_sub_epi32(e_est, c13), 23));
            const __m256 step    = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_and_si256(est, m3FF)), scale);
            const __m256 y_fi    = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_and_si256(ai, m7FFFF)), inv524k);
            fi_val = _mm256_sub_ps(base, _mm256_mul_ps(step, y_fi));
        }

        const __m256 nr_err  = nc_fnms(absW, fi_val, one, fused);
        const __m256 invW    = nc_fma(fi_val, nr_err, fi_val, fused);

        /* Clip flags and screen X, Y. */
        const __m256 w_neg_f = _mm256_castsi256_ps(_mm256_and_si256(
            _mm256_srai_epi32(_mm256_castps_si256(W), 31), one_bits));
        const __m256 far_f   = _mm256_cmp_ps(Zc, absW, _CMP_GT_OQ);
        const __m256 near_f  = _mm256_cmp_ps(negAbsW, Zc, _CMP_GT_OQ);
        const __m256 sy      = nc_fma(w_neg_f, offY, nc_fms(invW, Yc, c_1_64, fused), fused);
        const __m256 sx      = nc_fma(w_neg_f, offX, nc_fms(Xc, invW, c_1_64, fused), fused);

        /* Transpose SoA (sx, sy, near, far) -> AoS and store big-endian. */
        const __m256i o0 = _mm256_unpacklo_epi32(_mm256_castps_si256(sx), _mm256_castps_si256(sy));
        const __m256i o1 = _mm256_unpackhi_epi32(_mm256_castps_si256(sx), _mm256_castps_si256(sy));
        const __m256i o2 = _mm256_unpacklo_epi32(_mm256_castps_si256(near_f), _mm256_castps_si256(far_f));
        const __m256i o3 = _mm256_unpackhi_epi32(_mm256_castps_si256(near_f), _mm256_castps_si256(far_f));
        const __m256i v0 = _mm256_shuffle_epi8(_mm256_unpacklo_epi64(o0, o2), bsw);
        const __m256i v1 = _mm256_shuffle_epi8(_mm256_unpackhi_epi64(o0, o2), bsw);
        const __m256i v2 = _mm256_shuffle_epi8(_mm256_unpacklo_epi64(o1, o3), bsw);
        const __m256i v3 = _mm256_shuffle_epi8(_mm256_unpackhi_epi64(o1, o3), bsw);

        uint8_t* dp = ls + dst_base + 16u * v;
        _mm_storeu_si128((__m128i*)(dp + 0),  _mm256_castsi256_si128(v0));
        _mm_storeu_si128((__m128i*)(dp + 16), _mm256_castsi256_si128(v1));
        _mm_storeu_si128((__m128i*)(dp + 32), _mm256_castsi256_si128(v2));
        _mm_storeu_si128((__m128i*)(dp + 48), _mm256_castsi256_si128(v3));
        if (!tail4) {
            _mm_storeu_si128((__m128i*)(dp + 64),  _mm256_extracti128_si256(v0, 1));
            _mm_storeu_si128((__m128i*)(dp + 80),  _mm256_extracti128_si256(v1, 1));
            _mm_storeu_si128((__m128i*)(dp + 96),  _mm256_extracti128_si256(v2, 1));
            _mm_storeu_si128((__m128i*)(dp + 112), _mm256_extracti128_si256(v3, 1));
        }
    }
    return 1;
}

/* Native AVX2 vertex transform for b0196FF40 0xF970..0xFA78.
 * Transforms nv4 (multiple of 4) 16-byte vertices at ls[src_base] into
 * {ox, oy, oz, 0.0f} at ls[dst_base], bit-exact with 0xF970. */
static int nc_f970_transform(uint8_t* ls, uint32_t src_base, uint32_t dst_base, uint32_t nv4,
                             const spu_context* ctx, int fused)
{
    const __m256i bsw = _mm256_setr_epi8(
        3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12,
        3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
    const __m256 wx = _mm256_set1_ps(ctx->gpr[34]._f32[0]), wy = _mm256_set1_ps(ctx->gpr[28]._f32[0]);
    const __m256 wz = _mm256_set1_ps(ctx->gpr[37]._f32[0]), ww = _mm256_set1_ps(ctx->gpr[32]._f32[0]);
    const __m256 zx = _mm256_set1_ps(ctx->gpr[22]._f32[0]), zy = _mm256_set1_ps(ctx->gpr[21]._f32[0]);
    const __m256 zz = _mm256_set1_ps(ctx->gpr[6]._f32[0]),  zw = _mm256_set1_ps(ctx->gpr[38]._f32[0]);
    const __m256 yx = _mm256_set1_ps(ctx->gpr[39]._f32[0]), yy = _mm256_set1_ps(ctx->gpr[35]._f32[0]);
    const __m256 yz = _mm256_set1_ps(ctx->gpr[29]._f32[0]), yw = _mm256_set1_ps(ctx->gpr[23]._f32[0]);
    const __m256 xx = _mm256_set1_ps(ctx->gpr[2]._f32[0]),  xy = _mm256_set1_ps(ctx->gpr[42]._f32[0]);
    const __m256 xz = _mm256_set1_ps(ctx->gpr[27]._f32[0]), xw = _mm256_set1_ps(ctx->gpr[33]._f32[0]);
    const __m256 zflip = _mm256_castsi256_ps(_mm256_set1_epi32((int)ctx->gpr[45]._u32[0]));
    const __m256 c_sub = _mm256_set1_ps(ctx->gpr[11]._f32[0]);
    const __m256 one   = _mm256_set1_ps(ctx->gpr[13]._f32[0]);
    const __m256 abs_m = _mm256_castsi256_ps(_mm256_set1_epi32((int)ctx->gpr[25]._u32[0]));
    const __m256 sgn_m = _mm256_castsi256_ps(_mm256_set1_epi32((int)0x80000000u));
    const __m256 zero_f = _mm256_setzero_ps();
    const __m256i sgn_i = _mm256_set1_epi32((int)0x80000000u);
    const __m256i c253 = _mm256_set1_epi32(253), c13 = _mm256_set1_epi32(13), c14 = _mm256_set1_epi32(14);
    const __m256i mFF = _mm256_set1_epi32(0xFF), m1F = _mm256_set1_epi32(0x1F);
    const __m256i m3FF = _mm256_set1_epi32(0x3FF), m7FFFF = _mm256_set1_epi32(0x7FFFF);
    const __m256i mBase = _mm256_set1_epi32((int)0xFFFFFC00u);
    const __m256 inv524k = _mm256_set1_ps(1.0f / 524288.0f);
    const __m256i zero_i = _mm256_setzero_si256();

    for (uint32_t v = 0; v < nv4; v += 8) {
        const int tail4 = (v + 4u == nv4);
        const uint8_t* sp = ls + src_base + 16u * v;
        const uint8_t* sp2 = tail4 ? sp : (sp + 64);
        const __m256i r0 = _mm256_shuffle_epi8(_mm256_setr_m128i(
            _mm_loadu_si128((const __m128i*)(sp + 0)), _mm_loadu_si128((const __m128i*)(sp2 + 0))), bsw);
        const __m256i r1 = _mm256_shuffle_epi8(_mm256_setr_m128i(
            _mm_loadu_si128((const __m128i*)(sp + 16)), _mm_loadu_si128((const __m128i*)(sp2 + 16))), bsw);
        const __m256i r2 = _mm256_shuffle_epi8(_mm256_setr_m128i(
            _mm_loadu_si128((const __m128i*)(sp + 32)), _mm_loadu_si128((const __m128i*)(sp2 + 32))), bsw);
        const __m256i r3 = _mm256_shuffle_epi8(_mm256_setr_m128i(
            _mm_loadu_si128((const __m128i*)(sp + 48)), _mm_loadu_si128((const __m128i*)(sp2 + 48))), bsw);

        const __m256i t0 = _mm256_unpacklo_epi32(r0, r1), t1 = _mm256_unpackhi_epi32(r0, r1);
        const __m256i t2 = _mm256_unpacklo_epi32(r2, r3), t3 = _mm256_unpackhi_epi32(r2, r3);
        const __m256 X  = _mm256_castsi256_ps(_mm256_unpacklo_epi64(t0, t2));
        const __m256 Y  = _mm256_castsi256_ps(_mm256_unpackhi_epi64(t0, t2));
        const __m256 Z  = _mm256_castsi256_ps(_mm256_unpacklo_epi64(t1, t3));
        const __m256 Zf = _mm256_xor_ps(Z, zflip);

        const __m256 W  = nc_fma(Z,  wz, nc_fma(Y,  wy, nc_fma(X,  wx, ww, fused), fused), fused);
        const __m256 Zo = nc_fma(Zf, zz, nc_fma(Y,  zy, nc_fma(X,  zx, zw, fused), fused), fused);
        const __m256 Yo = nc_fma(Zf, yz, nc_fma(yy, Y,  nc_fma(X,  yx, yw, fused), fused), fused);
        const __m256 Xo = nc_fma(Zf, xz, nc_fma(Y,  xy, nc_fma(xx, X,  xw, fused), fused), fused);

        /* Signed frest(W) + fi(W, est) + 1 Newton-Raphson step. */
        const __m256i wi    = _mm256_castps_si256(W);
        const __m256i sign  = _mm256_and_si256(wi, sgn_i);
        const __m256i exp   = _mm256_and_si256(_mm256_srli_epi32(wi, 23), mFF);
        const __m256i fidx  = _mm256_and_si256(_mm256_srli_epi32(wi, 18), m1F);
        const __m256i diff  = _mm256_sub_epi32(c253, exp);
        const __m256i rexp  = _mm256_slli_epi32(_mm256_and_si256(diff, _mm256_cmpgt_epi32(diff, zero_i)), 23);
        const __m256i frac  = _mm256_i32gather_epi32((const int*)spu_frest_fraction_lut, fidx, 4);
        const __m256i est   = _mm256_blendv_epi8(
            _mm256_or_si256(sign, _mm256_or_si256(rexp, frac)),
            _mm256_or_si256(sign, _mm256_set1_epi32(0x7FFFFFFF)),
            _mm256_cmpeq_epi32(exp, zero_i));
        const __m256i e_est = _mm256_and_si256(_mm256_srli_epi32(est, 23), mFF);
        __m256 fi_val;
        if (__builtin_expect(_mm256_movemask_epi8(_mm256_cmpgt_epi32(c14, e_est)), 0)) {
            uint32_t a8[8], b8[8]; float f8[8];
            _mm256_storeu_si256((__m256i*)a8, wi);
            _mm256_storeu_si256((__m256i*)b8, est);
            for (int k = 0; k < 8; k++) {
                uint32_t rb = b8[k], ra = a8[k];
                uint32_t eb = (rb >> 23) & 0xFFu;
                uint32_t bb = rb & 0xFFFFFC00u;
                float b_f; memcpy(&b_f, &bb, 4);
                float st = spu__ldexpf((float)(rb & 0x3FFu), (int)eb - 127 - 13);
                if (b_f < 0.0f) st = -st;
                float yf = spu__ldexpf((float)(ra & 0x7FFFFu), -19);
                f8[k] = b_f - st * yf;
            }
            fi_val = _mm256_loadu_ps(f8);
        } else {
            const __m256 base  = _mm256_castsi256_ps(_mm256_and_si256(est, mBase));
            const __m256 scale = _mm256_castsi256_ps(_mm256_slli_epi32(_mm256_sub_epi32(e_est, c13), 23));
            __m256 step        = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_and_si256(est, m3FF)), scale);
            step = _mm256_xor_ps(step, _mm256_and_ps(_mm256_cmp_ps(base, zero_f, _CMP_LT_OQ), sgn_m));
            const __m256 y_fi  = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_and_si256(wi, m7FFFF)), inv524k);
            fi_val = _mm256_sub_ps(base, _mm256_mul_ps(step, y_fi));
        }

        const __m256 nr_err  = nc_fnms(fi_val, W, one, fused);
        const __m256 invW    = nc_fma(nr_err, fi_val, fi_val, fused);
        const __m256 absInvW = _mm256_and_ps(invW, abs_m);

        const __m256 ox = nc_fms(Xo, absInvW, c_sub, fused);
        const __m256 oy = nc_fms(Yo, absInvW, c_sub, fused);
        const __m256 oz = nc_fms(Zo, absInvW, c_sub, fused);

        const __m256i o0 = _mm256_unpacklo_epi32(_mm256_castps_si256(ox), _mm256_castps_si256(oy));
        const __m256i o1 = _mm256_unpackhi_epi32(_mm256_castps_si256(ox), _mm256_castps_si256(oy));
        const __m256i o2 = _mm256_unpacklo_epi32(_mm256_castps_si256(oz), zero_i);
        const __m256i o3 = _mm256_unpackhi_epi32(_mm256_castps_si256(oz), zero_i);
        const __m256i v0 = _mm256_shuffle_epi8(_mm256_unpacklo_epi64(o0, o2), bsw);
        const __m256i v1 = _mm256_shuffle_epi8(_mm256_unpackhi_epi64(o0, o2), bsw);
        const __m256i v2 = _mm256_shuffle_epi8(_mm256_unpacklo_epi64(o1, o3), bsw);
        const __m256i v3 = _mm256_shuffle_epi8(_mm256_unpackhi_epi64(o1, o3), bsw);

        uint8_t* dp = ls + dst_base + 16u * v;
        _mm_storeu_si128((__m128i*)(dp + 0),  _mm256_castsi256_si128(v0));
        _mm_storeu_si128((__m128i*)(dp + 16), _mm256_castsi256_si128(v1));
        _mm_storeu_si128((__m128i*)(dp + 32), _mm256_castsi256_si128(v2));
        _mm_storeu_si128((__m128i*)(dp + 48), _mm256_castsi256_si128(v3));
        if (!tail4) {
            _mm_storeu_si128((__m128i*)(dp + 64),  _mm256_extracti128_si256(v0, 1));
            _mm_storeu_si128((__m128i*)(dp + 80),  _mm256_extracti128_si256(v1, 1));
            _mm_storeu_si128((__m128i*)(dp + 96),  _mm256_extracti128_si256(v2, 1));
            _mm_storeu_si128((__m128i*)(dp + 112), _mm256_extracti128_si256(v3, 1));
        }
    }
    return 1;
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

static int g_nv_on = -1;
static __attribute__((noinline)) int nv_init(void)
{
    const char* e = getenv("GT5P_NATIVE_VTX");
    int on = !(e && e[0] == '0');
    if (on && !nc_cpu_ok()) on = 0;
#if defined(NC_FORCE_OFF) || defined(NC_VTX_FORCE_OFF)
    on = 0;
#endif
    g_nv_on = on;
    return on;
}

int gt5p_cull_ad30(spu_context* ctx, int fused)
{
    int on = g_nv_on;
    if (__builtin_expect(on < 0, 0)) on = nv_init();
    if (!on) return 0;
    if (__builtin_expect(ctx->int_enable && (ctx->event_status & ctx->event_mask), 0)) return 0;

    const int32_t neg_bytes = (int32_t)ctx->gpr[33]._u32[0];
    if (neg_bytes >= 0 || ((-neg_bytes) & 63) != 0) return 0;
    const uint32_t bytes = (uint32_t)(-neg_bytes);
    const uint32_t nv4 = bytes >> 4;
    if (nv4 == 0 || nv4 > 4096u) return 0;

    const uint32_t src_base = (ctx->gpr[10]._u32[0] + (uint32_t)neg_bytes) & SPU_LS_MASK;
    const uint32_t dst_base = (ctx->gpr[30]._u32[0] + (uint32_t)neg_bytes) & SPU_LS_MASK;
    if ((src_base & 15u) || (dst_base & 15u)) return 0;
    if (src_base + bytes > SPU_LS_SIZE || dst_base + bytes > SPU_LS_SIZE) return 0;
    /* Reject overlapping src/dst or non-standard step pointers. */
    if (src_base < dst_base + bytes && dst_base < src_base + bytes) return 0;
    if (ctx->gpr[36]._u32[0] != ctx->gpr[10]._u32[0] + 16u ||
        ctx->gpr[38]._u32[0] != ctx->gpr[10]._u32[0] + 32u ||
        ctx->gpr[40]._u32[0] != ctx->gpr[10]._u32[0] + 48u ||
        ctx->gpr[42]._u32[0] != ctx->gpr[30]._u32[0] + 16u ||
        ctx->gpr[44]._u32[0] != ctx->gpr[30]._u32[0] + 32u ||
        ctx->gpr[46]._u32[0] != ctx->gpr[30]._u32[0] + 48u) return 0;

    if (!nc_vtx_transform(ctx->ls, src_base, dst_base, nv4, ctx, fused)) return 0;

    /* Exact epilogue of 0xAD30 before 0xAEB4. */
    const u128 r3 = ctx->gpr[3], r5_in = ctx->gpr[5], r6_in = ctx->gpr[6], r7_in = ctx->gpr[7], r11 = ctx->gpr[11];
    const u128 r12 = spu_ila(0x10203);
    const u128 r50 = spu_fsmbi(0x0);
    const u128 r15_a = spu_ila(0xAF88);
    const u128 r35 = spu_fsmbi(0xBF80);
    const u128 r20_a = spu_ila(0xB210);
    const u128 r25_a = spu_ls_read128(ctx, r5_in._u32[0] + 0x0);
    const u128 r5 = spu_ilhu(0x3D00);
    const u128 r26_a = spu_ls_read128(ctx, 0x110F0);
    const u128 r13 = spu_il(0);
    const u128 r27_a = spu_ls_read128(ctx, r7_in._u32[0] + 0x0);
    const u128 r56 = spu_selb(r15_a, r20_a, r6_in);
    const u128 r15 = spu_fsmbi(0xBBBB);
    const u128 r28_a = spu_cgti(r7_in, 0);
    const u128 r7 = spu_ls_read128(ctx, 0x113A0);
    const u128 r29_a = spu_shli(r11, 3);
    const u128 r6 = spu_ilhu(0x3400);
    const u128 r51 = spu_shufb(r25_a, r25_a, r50);
    const u128 r31_a = spu_sfi(r11, 4);
    const u128 r10_a = spu_shlqbyi(r25_a, 1);
    const u128 r20 = spu_il(1);
    const u128 r26_b = spu_shufb(r25_a, r25_a, r26_a);
    const u128 r27_b = spu_and(r27_a, r28_a);
    const u128 r37 = spu_ls_read128(ctx, r3._u32[0] + 0x0);
    const u128 r28_b = spu_ai(r29_a, 31);
    const u128 r25 = spu_ls_read128(ctx, 0x11070);
    const u128 r29_b = spu_cgtbi(r10_a, 0);
    const u128 r10_b = spu_shufb(r27_b, r27_b, r50);
    const u128 r40 = spu_csflt(r26_b, 155);
    const u128 r26 = spu_ls_read128(ctx, 0x11650);
    const u128 r27 = spu_il(7);
    const u128 r52 = spu_shlqbyi(r29_b, 1);
    const u128 r41 = spu_andi(r28_b, -32);
    const u128 r36 = spu_shufb(r29_b, r29_b, r50);
    const u128 r28 = spu_ila(0x10D40);
    const u128 r49 = spu_ls_read128(ctx, r3._u32[0] + 0x0);
    const u128 r10_c = spu_cgtbi(r10_b, 0);
    const u128 r29 = spu_ls_read128(ctx, 0x11060);
    const u128 r33_a = spu_andi(r31_a, 3);
    const u128 r31 = spu_rotqbyi(r25, 2);
    const u128 r10 = spu_andi(r10_c, 16);
    const u128 r32 = spu_rotqbyi(r13, 0);
    const u128 r39 = spu_shl(r20, r33_a);
    const u128 r33 = spu_fsmbi(0xFFFF);
    const u128 r53 = spu_ila(0xB5D8);

    ctx->gpr[5] = r5; ctx->gpr[6] = r6; ctx->gpr[7] = r7; ctx->gpr[10] = r10;
    ctx->gpr[12] = r12; ctx->gpr[13] = r13; ctx->gpr[15] = r15; ctx->gpr[20] = r20;
    ctx->gpr[25] = r25; ctx->gpr[26] = r26; ctx->gpr[27] = r27; ctx->gpr[28] = r28;
    ctx->gpr[29] = r29; ctx->gpr[31] = r31; ctx->gpr[32] = r32; ctx->gpr[33] = r33;
    ctx->gpr[35] = r35; ctx->gpr[36] = r36; ctx->gpr[37] = r37; ctx->gpr[39] = r39;
    ctx->gpr[40] = r40; ctx->gpr[41] = r41; ctx->gpr[49] = r49; ctx->gpr[50] = r50;
    ctx->gpr[51] = r51; ctx->gpr[52] = r52; ctx->gpr[53] = r53; ctx->gpr[56] = r56;
    return 1;
}

static int g_nf970_on = -1;
static __attribute__((noinline)) int nf970_init(void)
{
    const char* e = getenv("GT5P_NATIVE_F970");
    int on = !(e && e[0] == '0');
    if (on && !nc_cpu_ok()) on = 0;
#if defined(NC_FORCE_OFF) || defined(NC_F970_FORCE_OFF)
    on = 0;
#endif
    g_nf970_on = on;
    return on;
}

int gt5p_cull_f970(spu_context* ctx, int fused)
{
    int on = g_nf970_on;
    if (__builtin_expect(on < 0, 0)) on = nf970_init();
    if (!on) return 0;
    if (__builtin_expect(ctx->int_enable && (ctx->event_status & ctx->event_mask), 0)) return 0;

    const int32_t neg_bytes = (int32_t)ctx->gpr[31]._u32[0];
    if (neg_bytes >= 0 || neg_bytes <= -32768 || ((-neg_bytes) & 63) != 0) return 0;
    const uint32_t bytes = (uint32_t)(-neg_bytes);
    const uint32_t nv4 = bytes >> 4;
    if (nv4 == 0 || nv4 > 2048u) return 0;

    const uint32_t src_base = (ctx->gpr[8]._u32[0] + (uint32_t)neg_bytes) & SPU_LS_MASK;
    const uint32_t dst_base = (ctx->gpr[41]._u32[0] + (uint32_t)neg_bytes) & SPU_LS_MASK;
    if ((src_base & 15u) || (dst_base & 15u)) return 0;
    if (src_base + bytes > SPU_LS_SIZE || dst_base + bytes > SPU_LS_SIZE) return 0;
    if (src_base < dst_base + bytes && dst_base < src_base + bytes) return 0;
    if (ctx->gpr[53]._u32[0] != ctx->gpr[8]._u32[0] + 16u ||
        ctx->gpr[54]._u32[0] != ctx->gpr[8]._u32[0] + 32u ||
        ctx->gpr[55]._u32[0] != ctx->gpr[8]._u32[0] + 48u ||
        ctx->gpr[43]._u32[0] != ctx->gpr[41]._u32[0] + 16u ||
        ctx->gpr[49]._u32[0] != ctx->gpr[41]._u32[0] + 32u ||
        ctx->gpr[51]._u32[0] != ctx->gpr[41]._u32[0] + 48u ||
        ctx->gpr[16]._u32[0] != 0u) return 0;

    if (!nc_f970_transform(ctx->ls, src_base, dst_base, nv4, ctx, fused)) return 0;

    /* Exact epilogue of 0xF970 before 0xFA78. */
    const u128 r7 = ctx->gpr[7], r9_in = ctx->gpr[9], r12_in = ctx->gpr[12];
    const u128 r24_in = ctx->gpr[24], r26 = ctx->gpr[26];
    const u128 r2_a = spu_il(1);
    const u128 r21 = spu_mpyi(r26, -16);
    const u128 r22 = spu_ls_read128(ctx, r7._u32[0] + 0x0);
    const u128 r13_a = spu_shl(r2_a, r24_in);
    const u128 r23 = spu_ls_read128(ctx, r7._u32[0] + 0x0);
    const u128 r8 = spu_il(0);
    const u128 r24 = spu_ls_read128(ctx, r7._u32[0] + 0x0);
    const u128 r16_a = spu_shl(r2_a, r12_in);
    const u128 r6 = spu_ls_read128(ctx, r7._u32[0] + 0x0);
    const u128 r2 = spu_il(1);
    const u128 r25 = spu_ai(r13_a, -1);
    const u128 r27 = spu_ilhu(0x7F00);
    const u128 r12 = spu_rotqbyi(r12_in, 0);
    const u128 r28 = spu_ilhu(0xFF00);
    const u128 r13 = spu_rotqbyi(r9_in, 0);
    const u128 r16 = spu_ai(r16_a, -1);
    const u128 r9 = spu_fsm(r25);

    ctx->gpr[2] = r2; ctx->gpr[6] = r6; ctx->gpr[8] = r8; ctx->gpr[9] = r9;
    ctx->gpr[12] = r12; ctx->gpr[13] = r13; ctx->gpr[16] = r16; ctx->gpr[21] = r21;
    ctx->gpr[22] = r22; ctx->gpr[23] = r23; ctx->gpr[24] = r24; ctx->gpr[25] = r25;
    ctx->gpr[27] = r27; ctx->gpr[28] = r28;
    return 1;
}

/* Native triangle-strip index decompressor for b0196FF40 / 2.11 b017BD780
 * 0xA3C0 -> 0xA408 (RET hook). Unpacks nibble-coded strip control bytes at r4
 * and big-endian 16-bit vertex indices at r3 into 16-byte-aligned output at r5
 * directly in big-endian LS byte order using SSSE3 palignr + pshufb (no bswaps).
 * Rollback: GT5P_NATIVE_A3C0=0. */
static __attribute__((target("sse4.1"))) void nc_a3c0_run(
    uint8_t* ls, uint32_t src, uint32_t ctrl, uint32_t dst, uint32_t nblocks)
{
    static const uint8_t adv[16] = {
        4, 4, 8, 8, 4, 4, 8, 8, 8, 8, 12, 12, 8, 8, 12, 12
    };
    static const uint8_t lut[16][16] = {
        {0x00, 0x01, 0x04, 0x05, 0x08, 0x09, 0x00, 0x01, 0x00, 0x01, 0x08, 0x09, 0x0A, 0x0B, 0x00, 0x01},
        {0x00, 0x01, 0x04, 0x05, 0x08, 0x09, 0x00, 0x01, 0x08, 0x09, 0x04, 0x05, 0x0A, 0x0B, 0x08, 0x09},
        {0x00, 0x01, 0x04, 0x05, 0x08, 0x09, 0x00, 0x01, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x0A, 0x0B},
        {0x00, 0x01, 0x04, 0x05, 0x08, 0x09, 0x00, 0x01, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x0A, 0x0B},
        {0x04, 0x05, 0x02, 0x03, 0x08, 0x09, 0x04, 0x05, 0x04, 0x05, 0x08, 0x09, 0x0A, 0x0B, 0x04, 0x05},
        {0x04, 0x05, 0x02, 0x03, 0x08, 0x09, 0x04, 0x05, 0x08, 0x09, 0x02, 0x03, 0x0A, 0x0B, 0x08, 0x09},
        {0x04, 0x05, 0x02, 0x03, 0x08, 0x09, 0x04, 0x05, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x0A, 0x0B},
        {0x04, 0x05, 0x02, 0x03, 0x08, 0x09, 0x04, 0x05, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x0A, 0x0B},
        {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x00, 0x01, 0x00, 0x01, 0x04, 0x05, 0x06, 0x07, 0x00, 0x01},
        {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x00, 0x01, 0x04, 0x05, 0x02, 0x03, 0x06, 0x07, 0x04, 0x05},
        {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x00, 0x01, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x06, 0x07},
        {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x00, 0x01, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x06, 0x07},
        {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x00, 0x01, 0x00, 0x01, 0x04, 0x05, 0x06, 0x07, 0x00, 0x01},
        {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x00, 0x01, 0x04, 0x05, 0x02, 0x03, 0x06, 0x07, 0x04, 0x05},
        {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x00, 0x01, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x06, 0x07},
        {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x00, 0x01, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x06, 0x07}
    };
    __m128i prev = _mm_setzero_si128();
    const uint8_t* cp = ls + ctrl;
    uint8_t* dp = ls + dst;
    for (uint32_t k = 0; k < nblocks; k++) {
        const uint8_t b = cp[k];
        const unsigned hi = b >> 4;
        const unsigned lo = b & 15u;

        __m128i cur0 = _mm_loadu_si128((const __m128i*)(ls + src));
        __m128i win0 = (hi < 8u) ? _mm_alignr_epi8(cur0, prev, 8) : cur0;
        __m128i out0 = _mm_shuffle_epi8(win0, _mm_loadu_si128((const __m128i*)lut[hi]));
        src += adv[hi];

        __m128i cur1 = _mm_loadu_si128((const __m128i*)(ls + src));
        __m128i win1 = (lo < 8u) ? _mm_alignr_epi8(cur1, out0, 8) : cur1;
        __m128i out1 = _mm_shuffle_epi8(win1, _mm_loadu_si128((const __m128i*)lut[lo]));
        src += adv[lo];

        _mm_store_si128((__m128i*)(dp + 0), out0);
        _mm_store_si128((__m128i*)(dp + 16), out1);
        dp += 32;
        prev = out1;
    }
}

static int g_na3c0_on = -1;
static __attribute__((noinline)) int na3c0_init(void)
{
    const char* e = getenv("GT5P_NATIVE_A3C0");
    int on = !(e && e[0] == '0');
#if defined(NC_FORCE_OFF) || defined(NC_A3C0_FORCE_OFF)
    on = 0;
#endif
    g_na3c0_on = on;
    return on;
}

int gt5p_cull_a3c0(spu_context* ctx, int fused)
{
    (void)fused;
    int on = g_na3c0_on;
    if (__builtin_expect(on < 0, 0)) on = na3c0_init();
    if (!on) return 0;
    if (__builtin_expect(ctx->int_enable && (ctx->event_status & ctx->event_mask), 0)) return 0;

    const int32_t r6_in = (int32_t)ctx->gpr[6]._u32[0];
    if (r6_in <= 0 || r6_in > 32768) return 0;
    const uint32_t nblocks = (uint32_t)((r6_in + 3) >> 2);
    if (nblocks == 0) return 0;

    const uint32_t src = ctx->gpr[3]._u32[0] & SPU_LS_MASK;
    const uint32_t ctrl = ctx->gpr[4]._u32[0] & SPU_LS_MASK;
    const uint32_t dst = ctx->gpr[5]._u32[0] & 0x3FFF0u;
    const uint32_t src_max = nblocks * 24u + 16u;
    const uint32_t dst_bytes = nblocks * 32u;
    if (src + src_max > SPU_LS_SIZE || dst + dst_bytes > SPU_LS_SIZE || ctrl + nblocks > SPU_LS_SIZE)
        return 0;
    if (dst < src + src_max && src < dst + dst_bytes) return 0;
    if (dst < ctrl + nblocks && ctrl < dst + dst_bytes) return 0;
    if ((ctx->ls[ctrl] >> 4) < 8u) return 0;

    nc_a3c0_run(ctx->ls, src, ctrl, dst, nblocks);
    return 1;
}

/* Native RSX CMP (10_11_11) normal/tangent vertex packer for b0196FF40 /
 * 2.11 b017BD780 0xC440 -> 0xC4C0 -> 0xC5A0 -> 0xC5E0 (RET hook).
 * Replaces the 60-register multi-pass unaligned read-modify-write loop with a
 * single pass per vertex. Rollback: GT5P_NATIVE_C440=0. */
static __attribute__((target("sse4.1"))) void nc_c440_sse(
    uint8_t* ls, uint32_t src, uint32_t dst, uint32_t stride,
    uint32_t idx_ptr, uint32_t count, u128 scale, u128 bias)
{
    const __m128i bswap = _mm_setr_epi8(3,2,1,0, 7,6,5,4, 11,10,9,8, 15,14,13,12);
    const uint8_t* ip = ls + idx_ptr;
    uint8_t* dp = ls + dst;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t off = ((uint32_t)ip[0] << 8) | ip[1];
        ip += 2;
        u128 v;
        _mm_storeu_si128((__m128i*)v._u32,
            _mm_shuffle_epi8(_mm_load_si128((const __m128i*)(ls + ((src + off) & 0x3FFF0u))), bswap));
        u128 f = spu_fma(v, scale, bias);
        u128 s = spu_cflts(f, 142);
        uint32_t packed = (s._u32[2] & 0xFFC00000u)
                        | ((s._u32[1] >> 10) & 0x003FF800u)
                        | (s._u32[0] >> 21);
        uint32_t be = __builtin_bswap32(packed);
        memcpy(dp, &be, 4);
        dp += stride;
    }
}

static __attribute__((target("sse4.1,fma"))) void nc_c440_fma(
    uint8_t* ls, uint32_t src, uint32_t dst, uint32_t stride,
    uint32_t idx_ptr, uint32_t count, u128 scale, u128 bias)
{
    const __m128i bswap = _mm_setr_epi8(3,2,1,0, 7,6,5,4, 11,10,9,8, 15,14,13,12);
    const __m128 vscale = _mm_loadu_ps(scale._f32);
    const __m128 vbias  = _mm_loadu_ps(bias._f32);
    const uint8_t* ip = ls + idx_ptr;
    uint8_t* dp = ls + dst;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t off = ((uint32_t)ip[0] << 8) | ip[1];
        ip += 2;
        __m128 v = _mm_castsi128_ps(_mm_shuffle_epi8(
            _mm_load_si128((const __m128i*)(ls + ((src + off) & 0x3FFF0u))), bswap));
        u128 f;
        _mm_storeu_ps(f._f32, _mm_fmadd_ps(v, vscale, vbias));
        u128 s = spu_cflts(f, 142);
        uint32_t packed = (s._u32[2] & 0xFFC00000u)
                        | ((s._u32[1] >> 10) & 0x003FF800u)
                        | (s._u32[0] >> 21);
        uint32_t be = __builtin_bswap32(packed);
        memcpy(dp, &be, 4);
        dp += stride;
    }
}

static __attribute__((target("sse4.1,fma"))) u128 nc_fma_one(u128 a, u128 b, u128 c)
{
    u128 r;
    _mm_storeu_ps(r._f32, _mm_fmadd_ps(_mm_loadu_ps(a._f32), _mm_loadu_ps(b._f32), _mm_loadu_ps(c._f32)));
    return r;
}

static int g_nc440_on = -1;
static __attribute__((noinline)) int nc440_init(void)
{
    const char* e = getenv("GT5P_NATIVE_C440");
    int on = !(e && e[0] == '0');
#if defined(NC_FORCE_OFF) || defined(NC_C440_FORCE_OFF)
    on = 0;
#endif
    g_nc440_on = on;
    return on;
}

int gt5p_cull_c440(spu_context* ctx, int fused)
{
    int on = g_nc440_on;
    if (__builtin_expect(on < 0, 0)) on = nc440_init();
    if (!on) return 0;
    if (__builtin_expect(ctx->int_enable && (ctx->event_status & ctx->event_mask), 0)) return 0;

    const uint32_t count = ctx->gpr[8]._u32[0];
    if (count == 0u) return 1;
    if (count > 16384u) return 0;

    const uint32_t src = ctx->gpr[3]._u32[0] & SPU_LS_MASK;
    const uint32_t dst = ctx->gpr[4]._u32[0] & SPU_LS_MASK;
    const uint32_t stride = ctx->gpr[5]._u32[0];
    const uint32_t idx_ptr = ctx->gpr[7]._u32[0] & SPU_LS_MASK;
    if (stride == 0u || stride > 256u || (stride & 3u) != 0u || (dst & 3u) != 0u || (idx_ptr & 15u) != 0u) return 0;

    const uint64_t dst_end = (uint64_t)dst + (uint64_t)(count - 1u) * stride + 4u;
    const uint64_t idx_end = (uint64_t)idx_ptr + (uint64_t)count * 2u;
    if (dst_end > SPU_LS_SIZE || idx_end > SPU_LS_SIZE) return 0;
    if (dst < idx_end && idx_ptr < dst_end) return 0;

    uint32_t or_bits = 0u, max_off = 0u;
    const uint8_t* ip = ctx->ls + idx_ptr;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t off = ((uint32_t)ip[2*i] << 8) | ip[2*i + 1];
        or_bits |= off;
        if (off > max_off) max_off = off;
    }
    if ((or_bits & 0x8003u) != 0u) return 0;
    const uint64_t src_end = (uint64_t)(src & 0x3FFF0u) + max_off + 16u;
    if (src_end > SPU_LS_SIZE) return 0;
    if (dst < src_end && (src & 0x3FFF0u) < dst_end) return 0;

    u128 c_bc0, c_bd0;
    c_bc0._u32[0] = 0x3F7FE000u; c_bc0._u32[1] = 0x3F7FE000u;
    c_bc0._u32[2] = 0x3F7FC000u; c_bc0._u32[3] = 0u;
    c_bd0._u32[0] = 0xBA000000u; c_bd0._u32[1] = 0xBA000000u;
    c_bd0._u32[2] = 0xBA800000u; c_bd0._u32[3] = 0u;

    const u128 scale = spu_fm(c_bc0, ctx->gpr[9]);
    const u128 bias  = fused ? nc_fma_one(ctx->gpr[10], c_bc0, c_bd0)
                             : spu_fma(ctx->gpr[10], c_bc0, c_bd0);

    if (fused)
        nc_c440_fma(ctx->ls, src, dst, stride, idx_ptr, count, scale, bias);
    else
        nc_c440_sse(ctx->ls, src, dst, stride, idx_ptr, count, scale, bias);
    return 1;
}
