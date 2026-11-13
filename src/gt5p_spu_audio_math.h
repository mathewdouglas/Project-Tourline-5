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
#if GT5P_AUDIO_EXTENDED_ZERO
#define spu_fi gt5p_audio_base_fi
#define spu_fm gt5p_audio_base_fm
#define spu_fma gt5p_audio_base_fma
#define spu_fms gt5p_audio_base_fms
#define spu_fnms gt5p_audio_base_fnms
#include "spu_helpers.h"
#undef spu_fi
#undef spu_fm
#undef spu_fma
#undef spu_fms
#undef spu_fnms

static inline int gt5p_audio_zero_extended(uint32_t a, uint32_t b)
{
    const uint32_t ae = a & 0x7F800000u, be = b & 0x7F800000u;
    return (ae == 0x7F800000u && be == 0) ||
           (be == 0x7F800000u && ae == 0);
}

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
    return r;
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
    return r;
}

static inline u128 spu_fms(u128 a, u128 b, u128 c)
{
    u128 r = gt5p_audio_base_fms(a, b, c);
    for (int i = 0; i < 4; ++i)
        if (gt5p_audio_zero_extended(a._u32[i], b._u32[i]))
            r._u32[i] = gt5p_audio_addend(c._u32[i] ^ 0x80000000u);
    return r;
}

static inline u128 spu_fnms(u128 a, u128 b, u128 c)
{
    u128 r = gt5p_audio_base_fnms(a, b, c);
    for (int i = 0; i < 4; ++i)
        if (gt5p_audio_zero_extended(a._u32[i], b._u32[i]))
            r._u32[i] = gt5p_audio_addend(c._u32[i]);
    return r;
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
