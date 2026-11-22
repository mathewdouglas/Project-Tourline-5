/*
 * gt5p_native_mixer.c -- native fast blocks for the hot loops of GT5's sound mixer SPU job
 * (socisynth, image jD58063CD): the two biquads (0x4720 in place, 0x4C80), the resampler
 * (0x44CC), the bus mix (0x4E58) and the ADPCM block loop (0x1908 + 0x1A2C).
 *
 * The fast blocks are gt5-recomp's (MIT License, Copyright (c) 2026 MormonFTP-hub,
 * GT5/src/native/native_mixer.c, v0.1.91), unchanged but for this runtime's local-store
 * accessor; the header, the hooks and the switch are written for this port.
 *
 * Each block advances its lifted loop by whole passes and writes only what those passes leave
 * behind (the loop-carried registers and the words they store), then the lifted function runs
 * the remaining passes (at least one) and its epilogue. Floats: the same host operations, in the
 * same order and fusion, as the lifted code does them (spu_fm = mul, spu_fma/fnms = FMA in this
 * FMA build), so finite values give the same bits in any rounding mode. A block stops before
 * committing any word with exponent 255 and leaves that pass to the lifted code, which applies
 * this port's own rules to it (src/gt5p_spu_audio_math.h).
 *
 * Wiring (CMakeLists.txt): only the FMA copy of the image (audio_native_jD58063CD.c,
 * gt5p_audio_ prefix, chosen at start on CPUs with AVX and FMA) is hooked. Its source is a copy
 * of the lifted file with these five definitions renamed ..._orig, and this file defines the
 * original names, so the lifted callers and the registry reach it. The portable copy is untouched.
 * GT5P_NATIVE_MIXER=0: every call goes straight to the lifted function.
 * Test: scripts/test/native_mixer.py (captured states, native against lifted, all registers and
 * the whole local store).
 */
#include "spu_recomp.h"
#include "spu_helpers.h"
#include <immintrin.h>
#include <stdlib.h>
#include <string.h>

#if !defined(__FMA__) || !defined(__SSE4_1__)
#error "gt5p_native_mixer.c is built like the FMA copy of the image: -msse4.1 -mfma"
#endif

#ifndef NM_PREFIX
#define NM_PREFIX gt5p_audio_jD58063CD_spu_func_
#endif
#define NM_CAT2(a, b) a##b
#define NM_CAT(a, b) NM_CAT2(a, b)
#define NATIVE_SV(A) NM_CAT(NM_PREFIX, A)
#define ORIG_SV(A)   NM_CAT(NM_CAT(NM_PREFIX, A), _orig)

#define NM_INLINE static inline __attribute__((always_inline))
#define NM_LS_QMASK (SPU_LS_MASK & ~0xFu)
#define NM_STAT(fn, field, n) ((void)0)

/* local store is big-endian; host lane i = SPU word i */
NM_INLINE __m128i nm_bswap(__m128i x) {
    return _mm_shuffle_epi8(x, _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12));
}
NM_INLINE __m128 nm_ldq(const uint8_t* p) { return _mm_castsi128_ps(nm_bswap(_mm_loadu_si128((const __m128i*)p))); }
NM_INLINE void nm_stq(uint8_t* p, __m128 v) { _mm_storeu_si128((__m128i*)p, nm_bswap(_mm_castps_si128(v))); }
NM_INLINE __m128 nm_reg(u128 a) { return _mm_loadu_ps((const float*)a._u32); }
NM_INLINE u128 nm_u128(__m128 v) { u128 r; _mm_storeu_ps((float*)r._u32, v); return r; }
/* any lane with exponent 255 (infinity or NaN to the host) */
NM_INLINE int nm_nonfinite(__m128 v) {
    const __m128i m = _mm_set1_epi32(0x7F800000);
    return _mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(_mm_and_si128(_mm_castps_si128(v), m), m)));
}

/* ---------------------------------------------------------------------------------------------
 * Biquads (0x4720 in place, 0x4C80 from one buffer to another or in place).
 *
 * One pass of the lifted loop, on all four lanes of the registers:
 *     x  = rotqby(quadword holding the sample, its address)      lane 0 = the sample
 *     y  = (((x1*b1 + b0*x) + x2*b2) - y1*a1) - y2*a2            1 mul + 4 FMA, each rounded
 *     x2 = x1; x1 = x; y2 = y1; y1 = y
 *     the sample's word in the output quadword = y lane 0
 * Lanes 1-3 of x are the words that follow the sample in its 16-byte line, wrapping to the line's
 * start, AS THE LINE STANDS at that pass.  They run the same recurrence on their own histories
 * and nothing ever reads them back into lane 0 -- but the lifted function writes all four lanes
 * of x, y and the state registers to ctx->gpr[], so they are computed here too.
 *
 * In place, with the line 16-byte aligned and words x0..x3 -> y0..y3, the four passes see
 *     [x0 x1 x2 x3]  [x1 x2 x3 y0]  [x2 x3 y0 y1]  [x3 y0 y1 y2]
 * The lifted loop is slow because each pass's vector waits for the previous pass's whole chain
 * (the y it just inserted into the line comes back in lane 3, 2, 1) and the line makes a round
 * trip through a shuffle and a blend.  Here lane 0 runs ahead as a scalar chain (its recurrence
 * is the two FMAs through y1/y2), and the four-lane chain follows, fed with those scalars: its
 * own recurrence is also just two FMAs.  Its lane 0 repeats the scalar chain (same operations on
 * the same values), which keeps the four-lane state registers whole.
 *
 * Out of place (0x4C80 with distinct lines) the passes see the four rotations of the input line,
 * no y involved, so the four-lane chain alone is enough.
 *
 * (-a*b + c): the lifted code negates one factor and uses fmadd; for finite values that is the
 * same number as fnmadd, which keeps the negation off the recurrence.
 * ------------------------------------------------------------------------------------------- */
#define NM_BQ_V(y, x) do {                                     \
        __m128 t_ = _mm_mul_ps(vx1, b1);                       \
        t_ = _mm_fmadd_ps(b0, (x), t_);                        \
        t_ = _mm_fmadd_ps(vx2, b2, t_);                        \
        t_ = _mm_fnmadd_ps(vy1, a1, t_);                       \
        (y) = _mm_fnmadd_ps(vy2, a2, t_);                      \
        vx2 = vx1; vx1 = (x); vy2 = vy1; vy1 = (y);            \
    } while (0)
#define NM_BQ_S(y, x) do {                                     \
        __m128 t_ = _mm_mul_ss(sx1, b1);                       \
        t_ = _mm_fmadd_ss(b0, (x), t_);                        \
        t_ = _mm_fmadd_ss(sx2, b2, t_);                        \
        t_ = _mm_fnmadd_ss(sy1, a1, t_);                       \
        (y) = _mm_fnmadd_ss(sy2, a2, t_);                      \
        sx2 = sx1; sx1 = (x); sy2 = sy1; sy1 = (y);            \
    } while (0)

typedef struct nm_bq { __m128 x1, x2, y1, y2; } nm_bq;

/* nq whole lines in place at p (16-byte aligned LS address). Returns the lines done; fewer than
 * nq when a line would have produced a non-finite value (that line is left untouched). */
NM_INLINE uint32_t nm_biquad_inplace(uint8_t* p, uint32_t nq, const __m128 b0, const __m128 b1, const __m128 b2,
                                     const __m128 a1, const __m128 a2, nm_bq* st)
{
    __m128 vx1 = st->x1, vx2 = st->x2, vy1 = st->y1, vy2 = st->y2;
    __m128 sx1 = vx1, sx2 = vx2, sy1 = vy1, sy2 = vy2;          /* lane 0 of each */
    uint32_t q = 0;
    for (; q < nq; q++, p += 16) {
        const __m128 X  = nm_ldq(p);
        const __m128 X1 = _mm_shuffle_ps(X, X, _MM_SHUFFLE(0, 3, 2, 1));     /* x1 x2 x3 x0 */
        const __m128 X2 = _mm_movehl_ps(X, X);                               /* x2 x3 .. ..  */
        const __m128 X3 = _mm_shuffle_ps(X, X, _MM_SHUFFLE(3, 3, 3, 3));     /* x3 ..        */
        __m128 y0, y1, y2, y3;
        NM_BQ_S(y0, X); NM_BQ_S(y1, X1); NM_BQ_S(y2, X2); NM_BQ_S(y3, X3);
        const __m128 Y01  = _mm_unpacklo_ps(y0, y1);                         /* y0 y1 .. ..  */
        const __m128 in1  = _mm_insert_ps(X1, y0, 0x30);                     /* x1 x2 x3 y0  */
        const __m128 in2  = _mm_shuffle_ps(X, Y01, _MM_SHUFFLE(1, 0, 3, 2)); /* x2 x3 y0 y1  */
        const __m128 Y012 = _mm_movelh_ps(Y01, y2);                          /* y0 y1 y2 ..  */
        const __m128 in3  = _mm_move_ss(_mm_shuffle_ps(Y012, Y012, _MM_SHUFFLE(2, 1, 0, 0)), X3); /* x3 y0 y1 y2 */
        __m128 v0, v1, v2, v3;
        const __m128 ox1 = vx1, ox2 = vx2, oy1 = vy1, oy2 = vy2;
        NM_BQ_V(v0, X); NM_BQ_V(v1, in1); NM_BQ_V(v2, in2); NM_BQ_V(v3, in3);
        /* A non-finite sample, state or coefficient in a lane makes that lane's y non-finite from
         * there on, so the last y of the line tells for the whole line. */
        if (__builtin_expect(nm_nonfinite(v3) != 0, 0)) { vx1 = ox1; vx2 = ox2; vy1 = oy1; vy2 = oy2; break; }
        nm_stq(p, _mm_movelh_ps(Y01, _mm_unpacklo_ps(y2, y3)));
    }
    st->x1 = vx1; st->x2 = vx2; st->y1 = vy1; st->y2 = vy2;
    return q;
}

/* nq whole lines from in to out (both 16-byte aligned LS addresses, not the same line). */
NM_INLINE uint32_t nm_biquad_copy(const uint8_t* in, uint8_t* out, uint32_t nq, const __m128 b0, const __m128 b1,
                                  const __m128 b2, const __m128 a1, const __m128 a2, nm_bq* st)
{
    __m128 vx1 = st->x1, vx2 = st->x2, vy1 = st->y1, vy2 = st->y2;
    uint32_t q = 0;
    for (; q < nq; q++, in += 16, out += 16) {
        const __m128 X   = nm_ldq(in);
        const __m128 in1 = _mm_shuffle_ps(X, X, _MM_SHUFFLE(0, 3, 2, 1));
        const __m128 in2 = _mm_shuffle_ps(X, X, _MM_SHUFFLE(1, 0, 3, 2));
        const __m128 in3 = _mm_shuffle_ps(X, X, _MM_SHUFFLE(2, 1, 0, 3));
        __m128 v0, v1, v2, v3;
        const __m128 ox1 = vx1, ox2 = vx2, oy1 = vy1, oy2 = vy2;
        NM_BQ_V(v0, X); NM_BQ_V(v1, in1); NM_BQ_V(v2, in2); NM_BQ_V(v3, in3);
        if (__builtin_expect(nm_nonfinite(v3) != 0, 0)) { vx1 = ox1; vx2 = ox2; vy1 = oy1; vy2 = oy2; break; }
        nm_stq(out, _mm_movelh_ps(_mm_unpacklo_ps(v0, v1), _mm_unpacklo_ps(v2, v3)));   /* lane 0 of each */
    }
    st->x1 = vx1; st->x2 = vx2; st->y1 = vy1; st->y2 = vy2;
    return q;
}

/* Lines the fast block may take: the loop runs span/4 passes; at least one is left to the lifted
 * function. 0 when the count is not a sane multiple of 4 (the lifted loop then runs on by wrapping
 * its 32-bit counter, which is its business). */
NM_INLINE uint32_t nm_lines(uint32_t span)
{
    if ((span & 3) || span < 20 || span > SPU_LS_SIZE) return 0;
    return (span / 4 - 1) / 4;
}

/* 0x4720: in-place biquad.  x1=r13 x2=r16 y1=r10 y2=r11  b0=r21 b1=r19 b2=r18 a1=r17 a2=r20
 * buffer = r14 + r7, r7 steps by 4 up to r22.  Loop-carried: r7 r10 r11 r13 r16. */
static void nm_fast_00004720(spu_context* ctx)
{
    u128* const g = ctx->gpr;
    const uint32_t a0 = g[7]._u32[0] + g[14]._u32[0];
    const uint32_t nq = nm_lines(g[22]._u32[0] - g[7]._u32[0]);
    const uint32_t a = a0 & SPU_LS_MASK;
    if (!nq || (a0 & 15) || a + 16u * nq > SPU_LS_SIZE) { NM_STAT(NATIVE_FN_4720, bail_precond, 1); return; }
    nm_bq st = { nm_reg(g[13]), nm_reg(g[16]), nm_reg(g[10]), nm_reg(g[11]) };
    const uint32_t done = nm_biquad_inplace(ctx->ls + a, nq, nm_reg(g[21]), nm_reg(g[19]), nm_reg(g[18]),
                                            nm_reg(g[17]), nm_reg(g[20]), &st);
    if (done) {
        g[7] = spu_ai(g[7], (int32_t)(16u * done));
        g[13] = nm_u128(st.x1); g[16] = nm_u128(st.x2); g[10] = nm_u128(st.y1); g[11] = nm_u128(st.y2);
        NM_STAT(NATIVE_FN_4720, fast_calls, 1); NM_STAT(NATIVE_FN_4720, fast_iters, 4u * done);
    }
    if (done != nq) NM_STAT(NATIVE_FN_4720, bail_nonfinite, 1);
}

/* 0x4C80: biquad from r17 + r7 to r14 + r7.  x1=r13 x2=r15 y1=r10 y2=r11  b0=r23 b1=r20 b2=r19
 * a1=r18 a2=r21.  Loop-carried: r7 r10 r11 r13 r15.  The mixer calls it once per enabled stage:
 * the first stage reads the voice buffer and writes the work buffer, later stages run in place
 * (the function sets r17 = r14 on its way out). */
static void nm_fast_00004C80(spu_context* ctx)
{
    u128* const g = ctx->gpr;
    const uint32_t i0 = g[7]._u32[0] + g[17]._u32[0], o0 = g[7]._u32[0] + g[14]._u32[0];
    const uint32_t nq = nm_lines(g[22]._u32[0] - g[7]._u32[0]);
    const uint32_t ia = i0 & SPU_LS_MASK, oa = o0 & SPU_LS_MASK;
    if (!nq || ((i0 | o0) & 15) || ia + 16u * nq > SPU_LS_SIZE || oa + 16u * nq > SPU_LS_SIZE) {
        NM_STAT(NATIVE_FN_4C80, bail_precond, 1); return;
    }
    nm_bq st = { nm_reg(g[13]), nm_reg(g[15]), nm_reg(g[10]), nm_reg(g[11]) };
    const __m128 b0 = nm_reg(g[23]), b1 = nm_reg(g[20]), b2 = nm_reg(g[19]), a1 = nm_reg(g[18]), a2 = nm_reg(g[21]);
    /* Both lines aligned: an output line is either the input line of the same pass (in place) or
     * a whole other line, written before any later pass reads it and after any earlier pass read
     * it -- the same order as the lifted loop's. */
    const uint32_t done = ia == oa ? nm_biquad_inplace(ctx->ls + ia, nq, b0, b1, b2, a1, a2, &st)
                                   : nm_biquad_copy(ctx->ls + ia, ctx->ls + oa, nq, b0, b1, b2, a1, a2, &st);
    if (done) {
        g[7] = spu_ai(g[7], (int32_t)(16u * done));
        g[13] = nm_u128(st.x1); g[15] = nm_u128(st.x2); g[10] = nm_u128(st.y1); g[11] = nm_u128(st.y2);
        NM_STAT(NATIVE_FN_4C80, fast_calls, 1); NM_STAT(NATIVE_FN_4C80, fast_iters, 4u * done);
    }
    if (done != nq) NM_STAT(NATIVE_FN_4C80, bail_nonfinite, 1);
}

/* ---------------------------------------------------------------------------------------------
 * 0x44CC: the resampler, an 8-tap polyphase FIR.  One output sample per pass:
 *     pos  = r17 + r12 (phase + accumulated pitch, 20.12 fixed point);  r12 += r21
 *     idx  = (pos & 4095) >> 2                       1024 sub-positions
 *     in   = the 8 words at  (r14 & ~15) + 4*(pos >> 12)      (three lines, picked apart with the
 *                                                              shuffle-control table at r20)
 *     a    = in[0..3] * coef[idx]  (fma)  +  in[4..7] * reverse(coef[1024-idx])  (mul)
 *     out  = (a0 + a2) + (a1 + a3)   ->  the word at r13 + r11;  r11 += 4 up to r15 (= 1024)
 * Loop-carried: r11 and r12 only; no float is carried from one pass to the next, so the fast
 * block is a plain loop the CPU overlaps freely.  Every pass still reads local store as it then
 * is and stores its one word straight away, in the lifted order, so overlapping buffers behave
 * the same.
 * ------------------------------------------------------------------------------------------- */
static void nm_fast_000044CC(spu_context* ctx)
{
    u128* const g = ctx->gpr;
    uint8_t* const ls = ctx->ls;
    uint32_t r11 = g[11]._u32[0], acc = g[12]._u32[0];
    const uint32_t ph = g[17]._u32[0], inc = g[21]._u32[0], in = g[14]._u32[0], out = g[13]._u32[0];
    const uint32_t ctab = g[16]._u32[0], stab = g[20]._u32[0];
    /* the constants the lifted loop takes from registers and tables: 1024 sub-positions, a 12-bit
     * fraction, the word-reversing shuffle control (LS 0x8CE0) and the four "8 words starting at
     * word w" controls (LS 0x8EB0). Anything else: not our loop. */
    int ok = g[15]._u32[0] == 1024 && g[19]._u32[0] == 4095 && !(r11 & 3) && r11 < 1024 - 4
          && g[18]._u32[0] == 0x0C0D0E0Fu && g[18]._u32[1] == 0x08090A0Bu
          && g[18]._u32[2] == 0x04050607u && g[18]._u32[3] == 0x00010203u;
    const uint32_t n = ok ? (1024 - r11) / 4 - 1 : 0;      /* passes for the fast block */
    const uint32_t o_lo = (r11 + out) & SPU_LS_MASK & ~3u, o_hi = o_lo + 4u * n;   /* words it stores */
    if (ok) {
        if (o_hi > SPU_LS_SIZE) ok = 0;                    /* output wraps: leave it */
        const __m128i iota = _mm_setr_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
        for (uint32_t w = 0; w < 4 && ok; w++) {
            const uint32_t t = (stab + 16u * w) & NM_LS_QMASK;
            const __m128i c = _mm_loadu_si128((const __m128i*)(ls + t));
            if (_mm_movemask_epi8(_mm_cmpeq_epi8(c, _mm_add_epi8(iota, _mm_set1_epi8((char)(4 * w))))) != 0xFFFF) ok = 0;
            if (t < o_hi && o_lo < t + 16) ok = 0;         /* the loop would overwrite its own table */
        }
    }
    if (!ok) { NM_STAT(NATIVE_FN_44CC, bail_precond, 1); return; }
    const __m128i rev = _mm_setr_epi8(15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0);
    uint8_t* op = ls + o_lo;
    uint32_t k = 0;
    for (; k < n; k++, op += 4) {
        const uint32_t pos = ph + acc;
        const uint32_t q0 = (in + ((uint32_t)((int32_t)pos >> 14) << 4)) & NM_LS_QMASK;
        if (__builtin_expect(q0 > SPU_LS_SIZE - 48, 0)) break;       /* the three lines wrap */
        const uint32_t idx = (pos & 4095u) >> 2;
        const uint8_t* const s = ls + q0 + ((pos >> 10) & 12u);
        const __m128 lo = nm_ldq(s), hi = nm_ldq(s + 16);
        const __m128 c1 = nm_ldq(ls + ((ctab + (idx << 4)) & NM_LS_QMASK));
        const __m128 c2 = _mm_castsi128_ps(_mm_shuffle_epi8(
            _mm_loadu_si128((const __m128i*)(ls + ((ctab + ((1024u - idx) << 4)) & NM_LS_QMASK))), rev));
        const __m128 a = _mm_fmadd_ps(lo, c1, _mm_mul_ps(hi, c2));
        const __m128 h = _mm_add_ps(a, _mm_movehl_ps(a, a));         /* a0+a2, a1+a3 */
        const __m128 o = _mm_add_ss(h, _mm_shuffle_ps(h, h, _MM_SHUFFLE(1, 1, 1, 1)));
        const uint32_t bits = (uint32_t)_mm_cvtsi128_si32(_mm_castps_si128(o));
        /* all 16 operands feed the one sum: a non-finite one makes it non-finite */
        if (__builtin_expect((bits & 0x7F800000u) == 0x7F800000u, 0)) break;
        const uint32_t be = SPU_BSWAP32(bits);
        memcpy(op, &be, 4);
        acc += inc;
    }
    if (k) {
        g[11] = spu_ai(g[11], (int32_t)(4u * k));
        for (int l = 0; l < 4; l++) g[12]._u32[l] += k * g[21]._u32[l];
        NM_STAT(NATIVE_FN_44CC, fast_calls, 1); NM_STAT(NATIVE_FN_44CC, fast_iters, k);
    }
    if (k != n) NM_STAT(NATIVE_FN_44CC, bail_nonfinite, 1);
}

/* ---------------------------------------------------------------------------------------------
 * 0x4E58: mix one voice line into one bus with a volume ramp, a line (4 samples) per pass:
 *     dst[r6] = src[r17 + r7] * gain + dst[r6];   gain += step;   r6 += 16; r7 += 16 up to r10
 * gain = r5, step = r8 (four lanes each).  Loop-carried: r5 r6 r7.
 * ------------------------------------------------------------------------------------------- */
static void nm_fast_00004E58(spu_context* ctx)
{
    u128* const g = ctx->gpr;
    uint8_t* const ls = ctx->ls;
    const uint32_t span = g[10]._u32[0] - g[7]._u32[0];
    const uint32_t s0 = g[7]._u32[0] + g[17]._u32[0], d0 = g[6]._u32[0];
    __m128 gain = nm_reg(g[5]);
    const __m128 step = nm_reg(g[8]);
    if ((span & 15) || span < 32 || span > SPU_LS_SIZE || nm_nonfinite(gain) || nm_nonfinite(step)) {
        NM_STAT(NATIVE_FN_4E58, bail_precond, 1); return;
    }
    const uint32_t n = span / 16 - 1;
    uint32_t k = 0;
    for (; k < n; k++) {
        uint8_t* const d = ls + ((d0 + 16u * k) & NM_LS_QMASK);
        const __m128 o = _mm_fmadd_ps(nm_ldq(ls + ((s0 + 16u * k) & NM_LS_QMASK)), gain, nm_ldq(d));
        if (__builtin_expect(nm_nonfinite(o) != 0, 0)) break;
        nm_stq(d, o);
        gain = _mm_add_ps(gain, step);
    }
    if (k) {
        g[5] = nm_u128(gain);
        g[6] = spu_ai(g[6], (int32_t)(16u * k));
        g[7] = spu_ai(g[7], (int32_t)(16u * k));
        NM_STAT(NATIVE_FN_4E58, fast_calls, 1); NM_STAT(NATIVE_FN_4E58, fast_iters, k);
    }
    if (k != n) NM_STAT(NATIVE_FN_4E58, bail_nonfinite, 1);
}

/* ---------------------------------------------------------------------------------------------
 * 0x1908 + 0x1A2C: the ADPCM decoder.  One 16-byte block -> 28 samples, r67 blocks:
 *     0x1908  byte 0 of the block at r66 = filter (high nibble) and shift (low nibble); the
 *             coefficient line c = LS[r70 + 16*filter]; the 28 nibbles of bytes 2..15, each sign
 *             extended, shifted and converted: d = nibble * 2^(-3-shift)
 *     0x1A2C  28 times, straight-line:  s = clamp((d + h0*ca) + h1*cb), lower bound r64, upper r63
 *             even sample: (ca, cb) = (c[0], c[1]), s replaces h1;  odd: (c[1], c[0]), s replaces h0
 *             (h0, h1 = words 0 and 1 of the history register r10; fm and fa, no fma)
 *             s -> the word at r62 + 4k;  then r62 += 112, r66 += r65, and back to 0x1908 if r67 != 0
 * The loop here is the block loop: carried from block to block are r10 (words 0/1 = the last two
 * samples; words 2/3 pass through untouched), r62, r66 and r67.  Everything else both functions
 * write is assigned again in every block.  So the hook sits on 0x1908: decode all blocks but the
 * last one here, then let the lifted pair do the last block.
 *
 * The lifted chain per sample is shufb (gather the history), fm, fa, fa, fcgt+selb, fcgt+selb,
 * shufb (put the sample back): about 41 cycles.  Here it is mul, add(, add), min, max on scalars.
 * min/max: (t > hi ? hi : t) is minss(hi, t) and (lo > v ? lo : v) is maxss(lo, v) operand for
 * operand, including which operand comes back on a tie, as long as no operand is a denormal
 * (the compares see denormals as zero): t is a flushed sum, so only the bounds need checking.
 * And the clamp is half of what is left of the chain, yet it only matters for a sample outside
 * [lo, hi]: each block is first decoded without it and kept if all 28 sums were inside (the
 * clamp would have returned each of them bit for bit); otherwise it is decoded again with it.
 * The nibble unpacking is not on that chain; it uses the lifted code's own helpers.
 * ------------------------------------------------------------------------------------------- */
static void nm_fast_00001908(spu_context* ctx)
{
    static const uint8_t swapctl[16] = { 4, 5, 6, 7, 0, 1, 2, 3, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80 };
    u128* const g = ctx->gpr;
    uint8_t* const ls = ctx->ls;
    const uint32_t nblk = g[67]._u32[0];
    const uint32_t o0 = g[62]._u32[0] & SPU_LS_MASK & ~3u;
    const __m128 hi = nm_reg(g[63]), lo = nm_reg(g[64]);
    const uint32_t hib = g[63]._u32[0], lob = g[64]._u32[0];
    /* The controls the lifted code takes from registers and from LS 0x8CB0: "replace word 0",
     * "replace word 1", "swap words 0 and 1, clear 2 and 3". Bounds: finite, not denormal, lo <= hi. */
    const int ok = nblk >= 2 && nblk <= 4096
        && g[68]._u32[0] == 0x00010203u && g[68]._u32[1] == 0x14151617u && g[68]._u32[2] == 0x18191A1Bu && g[68]._u32[3] == 0x1C1D1E1Fu
        && g[69]._u32[0] == 0x10111213u && g[69]._u32[1] == 0x00010203u && g[69]._u32[2] == 0x18191A1Bu && g[69]._u32[3] == 0x1C1D1E1Fu
        && !memcmp(ls + 0x8CB0, swapctl, 16)
        && (hib & 0x7F800000u) != 0x7F800000u && (lob & 0x7F800000u) != 0x7F800000u
        && ((hib & 0x7F800000u) || !(hib & 0x007FFFFFu)) && ((lob & 0x7F800000u) || !(lob & 0x007FFFFFu))
        && !(_mm_movemask_ps(_mm_cmpgt_ss(lo, hi)) & 1)
        && o0 + 112u * (nblk - 1) <= SPU_LS_SIZE
        && !(o0 < 0x8CC0u && o0 + 112u * (nblk - 1) > 0x8CB0u);     /* output on the control line */
    if (!ok) { NM_STAT(NATIVE_FN_1908, bail_precond, 1); return; }
    const u128 k0123 = spu_ila(0x10203);
    const __m128 hi4 = _mm_shuffle_ps(hi, hi, 0), lo4 = _mm_shuffle_ps(lo, lo, 0);
    int clipping = 0;
    const uint32_t stride = g[65]._u32[0], ctab = g[70]._u32[0];
    uint32_t r66 = g[66]._u32[0];
    __m128 h0 = nm_reg(g[10]), h1 = _mm_shuffle_ps(h0, h0, _MM_SHUFFLE(1, 1, 1, 1));
    uint8_t* op = ls + o0;
    uint32_t b = 0;
    for (; b < nblk - 1; b++, op += 112, r66 += stride) {
        /* as 0x1908 */
        const u128 r48 = spu_ls_read128(ctx, r66);
        const u128 r6 = spu_rotqbyi(r48, 13);
        const u128 r3 = spu_andi(r6, 255), r8 = spu_andi(r6, 15);
        const u128 r32 = spu_rothmi(r3, -4), r34 = spu_sfhi(r8, 0);
        const u128 r73 = spu_andi(r34, 255), r41 = spu_andi(r32, 255);
        const u128 r36 = spu_shufb(r73, r73, k0123);
        const u128 coef = spu_ls_read128(ctx, spu_shli(r41, 4)._u32[0] + ctab);
#define NM_NIB(n) spu_csflt(spu_rotma(spu_shli(spu_roti(r48, n), 28), r36), 124)
        const u128 n24 = NM_NIB(24), n20 = NM_NIB(20), n32 = NM_NIB(32), n28 = NM_NIB(28);
        const u128 n8 = NM_NIB(8), n4 = NM_NIB(4), n16 = NM_NIB(16), n12 = NM_NIB(12);
#undef NM_NIB
        float d[28], s_out[28];
        d[0] = n24._f32[0]; d[1] = n20._f32[0]; d[2] = n32._f32[0]; d[3] = n28._f32[0];
        for (int l = 1; l < 4; l++) {
            float* const q = d + 8 * l - 4;
            q[0] = n8._f32[l]; q[1] = n4._f32[l]; q[2] = n16._f32[l]; q[3] = n12._f32[l];
            q[4] = n24._f32[l]; q[5] = n20._f32[l]; q[6] = n32._f32[l]; q[7] = n28._f32[l];
        }
        const __m128 c0 = nm_reg(coef), c1 = _mm_shuffle_ps(c0, c0, _MM_SHUFFLE(1, 1, 1, 1));
        /* the two coefficients and the two history words are all that is not a nibble: finite
         * here, finite throughout (the history after the first block is our own output) */
        if (__builtin_expect((nm_nonfinite(c0) | nm_nonfinite(_mm_unpacklo_ps(h0, h1))) & 3, 0)) break;
        __m128 a0 = h0, a1 = h1;
        int redo = clipping;
        if (!redo) {
            /* First without the clamp, which is half of the chain: if no sample of the block
             * leaves [lo, hi], the clamp returned every sum unchanged and this IS the result. */
            for (int k = 0; k < 28; k += 2) {
                a1 = _mm_add_ss(_mm_add_ss(_mm_load_ss(d + k), _mm_mul_ss(a0, c0)), _mm_mul_ss(a1, c1));
                _mm_store_ss(s_out + k, a1);
                a0 = _mm_add_ss(_mm_add_ss(_mm_load_ss(d + k + 1), _mm_mul_ss(a0, c1)), _mm_mul_ss(a1, c0));
                _mm_store_ss(s_out + k + 1, a0);
            }
            __m128 out_of_range = _mm_setzero_ps();
            for (int k = 0; k < 28; k += 4) {
                const __m128 t = _mm_loadu_ps(s_out + k);
                out_of_range = _mm_or_ps(out_of_range, _mm_or_ps(_mm_cmplt_ps(hi4, t), _mm_cmpgt_ps(lo4, t)));
            }
            /* a clipping voice tends to keep clipping: skip the attempt for the rest of this call */
            if (_mm_movemask_ps(out_of_range)) { redo = clipping = 1; a0 = h0; a1 = h1; }
        }
        if (redo) {
            for (int k = 0; k < 28; k += 2) {
                a1 = _mm_max_ss(lo, _mm_min_ss(hi, _mm_add_ss(_mm_add_ss(_mm_load_ss(d + k), _mm_mul_ss(a0, c0)), _mm_mul_ss(a1, c1))));
                _mm_store_ss(s_out + k, a1);
                a0 = _mm_max_ss(lo, _mm_min_ss(hi, _mm_add_ss(_mm_add_ss(_mm_load_ss(d + k + 1), _mm_mul_ss(a0, c1)), _mm_mul_ss(a1, c0))));
                _mm_store_ss(s_out + k + 1, a0);
            }
        }
        /* belt and braces (it cannot happen under round toward zero): commit finite samples only */
        int nf = 0;
        for (int k = 0; k < 28; k += 4) nf |= nm_nonfinite(_mm_loadu_ps(s_out + k));
        if (__builtin_expect(nf != 0, 0)) break;
        for (int k = 0; k < 28; k += 4) nm_stq(op + 4 * k, _mm_loadu_ps(s_out + k));
        h0 = a0; h1 = a1;
        if (redo) NM_STAT(NATIVE_FN_1908, redone, 1); else NM_STAT(NATIVE_FN_1908, plain, 1);
    }
    if (b) {
        g[10]._u32[0] = (uint32_t)_mm_cvtsi128_si32(_mm_castps_si128(h0));
        g[10]._u32[1] = (uint32_t)_mm_cvtsi128_si32(_mm_castps_si128(h1));
        g[62] = spu_ai(g[62], (int32_t)(112u * b));
        for (int l = 0; l < 4; l++) g[66]._u32[l] += b * g[65]._u32[l];
        g[67] = spu_ai(g[67], -(int32_t)b);
        NM_STAT(NATIVE_FN_1908, fast_calls, 1); NM_STAT(NATIVE_FN_1908, fast_iters, 28u * b);
    }
    if (b != nblk - 1) NM_STAT(NATIVE_FN_1908, bail_nonfinite, 1);
}

/* ------------------------------------------------------------------------------------------- */

/* GT5P_NATIVE_MIXER: 1 (default) native fast blocks, 0 lifted only. Read once. */
static int nm_mode(void)
{
    static volatile int mode = -1;   /* benign race: every thread reads the same value */
    if (mode < 0) { const char* e = getenv("GT5P_NATIVE_MIXER"); mode = (e && *e == '0') ? 0 : 1; }
    return mode;
}

#define NM_ENTRY(A)                                                                    \
    void ORIG_SV(A)(spu_context* ctx);                                                 \
    void NATIVE_SV(A)(spu_context* ctx)                                                \
    {                                                                                  \
        if (__builtin_expect(nm_mode(), 1)) nm_fast_##A(ctx);                          \
        __attribute__((musttail)) return ORIG_SV(A)(ctx);                              \
    }
NM_ENTRY(00004720)
NM_ENTRY(00004C80)
NM_ENTRY(000044CC)
NM_ENTRY(00004E58)
NM_ENTRY(00001908)
