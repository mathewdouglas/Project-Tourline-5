/* Audio-only SPU extended/zero arithmetic compatibility.
 * Force-included before the lifted image's relative helper include. Ordinary
 * operands retain the runtime's existing arithmetic, including FMA dispatch.
 * SPU exponent 255 represents finite extended values, not IEEE NaN/Inf.
 * This handles the estimate/refine/sqrt(0) path measured in GT5's DSP init;
 * it is deliberately not a complete extended-range SPU implementation. */
#ifndef GT5P_SPU_AUDIO_MATH_H
#define GT5P_SPU_AUDIO_MATH_H
#include <stdlib.h>

#ifndef GT5P_AUDIO_EXTENDED_ZERO
#define GT5P_AUDIO_EXTENDED_ZERO 1
#endif
/* The extended-range recomputation below costs every float add, multiply and
 * compare a lane check: GT5 01.04's race audio jobs took 0.67 ms instead of
 * 0.38 and the job chain fell behind (about 17 underruns a second, 0.1.6 and
 * 0.1.7). Only targets whose audio needs it build it in
 * (audio_extended_range in config/targets.json: 2.11). */
#ifndef GT5P_AUDIO_EXTENDED_RANGE_BUILD
#define GT5P_AUDIO_EXTENDED_RANGE_BUILD 0
#endif
#if GT5P_AUDIO_EXTENDED_ZERO
#define spu_fi gt5p_audio_base_fi
#define spu_fm gt5p_audio_base_fm
#define spu_fma gt5p_audio_base_fma
#define spu_fms gt5p_audio_base_fms
#define spu_fnms gt5p_audio_base_fnms
#if GT5P_AUDIO_EXTENDED_RANGE_BUILD
#define spu_fa gt5p_audio_base_fa
#define spu_fs gt5p_audio_base_fs
#define spu_fceq gt5p_audio_base_fceq
#define spu_fcgt gt5p_audio_base_fcgt
#endif
#include "spu_helpers.h"
#undef spu_fi
#undef spu_fm
#undef spu_fma
#undef spu_fms
#undef spu_fnms
#if GT5P_AUDIO_EXTENDED_RANGE_BUILD
#undef spu_fa
#undef spu_fs
#undef spu_fceq
#undef spu_fcgt
#endif
#include <math.h>
#include <emmintrin.h>

static inline int gt5p_audio_zero_extended(uint32_t a, uint32_t b)
{
    const uint32_t ae = a & 0x7F800000u, be = b & 0x7F800000u;
    return (ae == 0x7F800000u && be == 0) ||
           (be == 0x7F800000u && ae == 0);
}

/* Extended range (GT5 2.11). On the SPU, exponent 255 is an ordinary finite
 * value up to about 2^129: a reciprocal estimate of 0 is one. Products and sums
 * with it stay finite, saturate at +-0x7FFFFFFF, flush below 2^-126 to +0 and
 * truncate. The host reads the same bits as NaN/Inf and spreads them: 2.11's
 * EQ (socepdideq) state filled with 0xFFFFFFFF that way and the race's surround
 * channel went NaN. Lanes with an exponent-255 operand or host result are
 * recomputed in double precision with the SPU's reading; every other lane
 * keeps the runtime's result. GT5P_AUDIO_EXTENDED_RANGE=0: the old behaviour
 * (only 0 x extended = 0). Not exact for fused ops near the rounding edge. */
#if GT5P_AUDIO_EXTENDED_RANGE_BUILD
static inline int gt5p_audio_range_on(void)
{
    static volatile int on = -1; /* benign race: every thread computes the same */
    if (on < 0) { const char* e = getenv("GT5P_AUDIO_EXTENDED_RANGE"); on = !(e && *e == '0'); }
    return on;
}
static inline int gt5p_audio_ext_lanes(const u128* v)
{
    __m128i x; memcpy(&x, v, sizeof x);
    const __m128i e = _mm_set1_epi32(0x7F800000);
    return _mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(_mm_and_si128(x, e), e)));
}
static inline double gt5p_spu_value(uint32_t w)
{
    const int e = (int)((w >> 23) & 0xFFu);
    if (!e) return 0.0;
    const double v = ldexp(1.0 + (double)(w & 0x7FFFFFu) * (1.0 / 8388608.0), e - 127);
    return (w >> 31) ? -v : v;
}
static inline uint32_t gt5p_spu_bits(double r)
{
    if (r == 0.0 || r != r) return 0;
    const uint32_t s = r < 0.0 ? 0x80000000u : 0u;
    const double a = fabs(r);
    if (a < 0x1p-126) return 0;
    if (a >= 0x1p129) return s | 0x7FFFFFFFu;
    int e; const double m = frexp(a, &e) * 2.0;   /* a = m * 2^(e-1), m in [1,2) */
    return s | ((uint32_t)(e - 1 + 127) << 23) | (uint32_t)((m - 1.0) * 8388608.0);
}
/* op: 0 a+b, 1 a-b, 2 a*b, 3 a*b+c, 4 a*b-c, 5 c-a*b */
static inline u128 gt5p_audio_range(u128 r, u128 a, u128 b, u128 c, int op)
{
    if (!gt5p_audio_range_on()) return r;
    const int lanes = gt5p_audio_ext_lanes(&a) | gt5p_audio_ext_lanes(&b) |
                      gt5p_audio_ext_lanes(&r) | (op >= 3 ? gt5p_audio_ext_lanes(&c) : 0);
    if (!lanes) return r;
    for (int i = 0; i < 4; ++i) {
        if (!(lanes & (1 << i))) continue;
        const double x = gt5p_spu_value(a._u32[i]), y = gt5p_spu_value(b._u32[i]);
        const double z = op >= 3 ? gt5p_spu_value(c._u32[i]) : 0.0;
        double v;
        switch (op) {
        case 0: v = x + y; break;
        case 1: v = x - y; break;
        case 2: v = x * y; break;
        case 3: v = x * y + z; break;
        case 4: v = x * y - z; break;
        default: v = z - x * y; break;
        }
        r._u32[i] = gt5p_spu_bits(v);
    }
    return r;
}
static inline u128 spu_fa(u128 a, u128 b) { return gt5p_audio_range(gt5p_audio_base_fa(a, b), a, b, a, 0); }
static inline u128 spu_fs(u128 a, u128 b) { return gt5p_audio_range(gt5p_audio_base_fs(a, b), a, b, a, 1); }
static inline u128 gt5p_audio_compare(u128 r, u128 a, u128 b, int gt)
{
    if (!gt5p_audio_range_on()) return r;
    const int lanes = gt5p_audio_ext_lanes(&a) | gt5p_audio_ext_lanes(&b);
    for (int i = 0; lanes && i < 4; ++i)
        if (lanes & (1 << i)) {
            const double x = gt5p_spu_value(a._u32[i]), y = gt5p_spu_value(b._u32[i]);
            r._u32[i] = (gt ? x > y : x == y) ? 0xFFFFFFFFu : 0u;
        }
    return r;
}
static inline u128 spu_fceq(u128 a, u128 b) { return gt5p_audio_compare(gt5p_audio_base_fceq(a, b), a, b, 0); }
static inline u128 spu_fcgt(u128 a, u128 b) { return gt5p_audio_compare(gt5p_audio_base_fcgt(a, b), a, b, 1); }
#else
static inline u128 gt5p_audio_range(u128 r, u128 a, u128 b, u128 c, int op)
{
    (void)a; (void)b; (void)c; (void)op;
    return r;
}
#endif

static inline u128 spu_fi(u128 a, u128 b)
{
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
    u128 r = gt5p_audio_base_fi(a, b);
    for (int i = 0; i < 4; ++i) {
        const uint32_t rb = b._u32[i];
        if ((rb & 0x7F800000u) == 0x7F800000u) {
            /* Interpolate a normalized significand, then restore the SPU
             * sign/exponent. Never interpret exponent 255 as host float. */
            uint32_t bits = (rb & 0x007FFC00u) | 0x3F800000u;
            float base; memcpy(&base, &bits, sizeof base);
            const float step = (float)(rb & 0x3FFu) * (1.0f / 8192.0f);
            const float y = (float)(a._u32[i] & 0x7FFFFu) * (1.0f / 524288.0f);
            const float correction = step * y;
            const float value = base - correction;
            memcpy(&bits, &value, sizeof bits);
            r._u32[i] = (rb & 0xFF800000u) | (bits & 0x007FFFFFu);
        }
    }
    return r;
}

static inline u128 spu_fm(u128 a, u128 b)
{
    u128 r = gt5p_audio_base_fm(a, b);
    for (int i = 0; i < 4; ++i)
        if (gt5p_audio_zero_extended(a._u32[i], b._u32[i])) r._u32[i] = 0;
    return gt5p_audio_range(r, a, b, a, 2);
}

/* The SPU flushes exponent-zero inputs. A zero product contributes nothing
 * to the addend, even when the other multiplicand is an extended value. */
static inline uint32_t gt5p_audio_addend(uint32_t c)
{
    return (c & 0x7F800000u) ? c : 0;
}

static inline u128 spu_fma(u128 a, u128 b, u128 c)
{
    u128 r = gt5p_audio_base_fma(a, b, c);
    for (int i = 0; i < 4; ++i)
        if (gt5p_audio_zero_extended(a._u32[i], b._u32[i]))
            r._u32[i] = gt5p_audio_addend(c._u32[i]);
    return gt5p_audio_range(r, a, b, c, 3);
}

static inline u128 spu_fms(u128 a, u128 b, u128 c)
{
    u128 r = gt5p_audio_base_fms(a, b, c);
    for (int i = 0; i < 4; ++i)
        if (gt5p_audio_zero_extended(a._u32[i], b._u32[i]))
            r._u32[i] = gt5p_audio_addend(c._u32[i] ^ 0x80000000u);
    return gt5p_audio_range(r, a, b, c, 4);
}

static inline u128 spu_fnms(u128 a, u128 b, u128 c)
{
    u128 r = gt5p_audio_base_fnms(a, b, c);
    for (int i = 0; i < 4; ++i)
        if (gt5p_audio_zero_extended(a._u32[i], b._u32[i]))
            r._u32[i] = gt5p_audio_addend(c._u32[i]);
    return gt5p_audio_range(r, a, b, c, 5);
}
#else
#include "spu_helpers.h"
#endif

/* SPU single precision truncates. RNE puts GT5's 1 Hz filter just outside
 * the stable range. Only the measured coefficient constructor calls enter
 * this scope; sample processing and render jobs keep their existing mode. */
static inline unsigned gt5p_spu_audio_round_enter(void)
{
#if defined(_M_X64) || defined(__x86_64__)
    const unsigned saved = _mm_getcsr();
    static volatile int on = -1; /* benign race: every thread computes the same */
    if (on < 0) {
        const char* e = getenv("GT5P_AUDIO_ROUND_ZERO");
        on = !(e && *e == '0');
    }
    if (on) _mm_setcsr((saved & ~0x6000u) | 0x6000u);
    return saved;
#else
    return 0;
#endif
}

static inline void gt5p_spu_audio_round_leave(unsigned saved)
{
#if defined(_M_X64) || defined(__x86_64__)
    _mm_setcsr(saved);
#else
    (void)saved;
#endif
}
#endif
