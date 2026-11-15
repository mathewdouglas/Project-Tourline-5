/* Synthetic SPU estimate/refine/zero regressions; contains no lifted code. */
#include "gt5p_spu_audio_math.h"
#include <assert.h>
#include <stdio.h>

static uint32_t seed = 0x32A71u;
static uint32_t random32(void) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; }
static void same(u128 a, u128 b) { assert(memcmp(&a, &b, sizeof a) == 0); }
static u128 splat(uint32_t bits) { u128 r; for (int i=0;i<4;i++) r._u32[i]=bits; return r; }

int main(void)
{
    /* All signed zero/subnormal operands are zero on the SPU, even when
     * paired with any exponent-255 estimate, in either operand position. */
    const uint32_t zero[] = {0, 0x80000000u, 1, 0x807FFFFFu};
    const uint32_t add[] = {0, 0x80000000u, 1, 0x807FFFFFu, 0x3F800001u, 0xBF000000u};
    for (unsigned k=0;k<4096;k++) {
        u128 extended=splat(0x7F800000u | (random32() & 0x807FFFFFu));
        for (unsigned z=0;z<sizeof zero/sizeof *zero;z++) {
            for (int swap=0;swap<2;swap++) {
                u128 a=swap?splat(zero[z]):extended, b=swap?extended:splat(zero[z]);
                same(spu_fm(a,b),splat(0));
                for (unsigned j=0;j<sizeof add/sizeof *add;j++) {
                    const uint32_t expected=(add[j]&0x7F800000u)?add[j]:0;
                    u128 c=splat(add[j]);
                    same(spu_fma(a,b,c),splat(expected));
                    same(spu_fnms(a,b,c),splat(expected));
                    same(spu_fms(a,b,c),splat(expected?expected^0x80000000u:0));
                }
            }
        }
    }
    for (unsigned k=0;k<100000;k++) {
        u128 a,b,c;
        for (int i=0;i<4;i++) {
            a._u32[i]=(random32()&0x807FFFFFu)|((80+random32()%90)<<23);
            b._u32[i]=(random32()&0x807FFFFFu)|((80+random32()%90)<<23);
            c._u32[i]=(random32()&0x807FFFFFu)|((80+random32()%90)<<23);
        }
        same(spu_fm(a,b),gt5p_audio_base_fm(a,b));
        same(spu_fma(a,b,c),gt5p_audio_base_fma(a,b,c));
        same(spu_fms(a,b,c),gt5p_audio_base_fms(a,b,c));
        same(spu_fnms(a,b,c),gt5p_audio_base_fnms(a,b,c));
        for (int i=0;i<4;i++) b._u32[i]=(random32()&0x807FFFFFu)|((14+random32()%241)<<23);
        same(spu_fi(a,b),gt5p_audio_base_fi(a,b));
    }
    /* Independently evaluate interpolation in double precision, rounding the
     * normalized result once, rather than interpreting an extended float. */
    for (unsigned k=0;k<100000;k++) {
        u128 a,b;
        for (int i=0;i<4;i++) { a._u32[i]=random32(); b._u32[i]=0x7F800000u|(random32()&0x807FFFFFu); }
        u128 r=spu_fi(a,b);
        for (int i=0;i<4;i++) {
            uint32_t bits=(b._u32[i]&0x007FFC00u)|0x3F800000u;
            float base; memcpy(&base,&bits,4);
            /* Match separate single-precision multiply and subtract. */
            float correction=(float)((double)(b._u32[i]&1023u)*(double)(a._u32[i]&0x7FFFFu)/4294967296.0);
            float value=(float)((double)base-(double)correction);
            memcpy(&bits,&value,4);
            assert(r._u32[i]==((b._u32[i]&0xFF800000u)|(bits&0x007FFFFFu)));
        }
    }
    /* Standard reciprocal-estimate Newton refinement used for sqrt(x).
     * Verify the exact-zero endpoint and unchanged positive lanes. */
    u128 x=splat(0); x._f32[1]=0.25f; x._f32[2]=1; x._f32[3]=4;
    u128 estimate=spu_frsqest(x), refined=spu_fi(x,estimate);
    u128 product=spu_fm(refined,x);
    u128 residual=spu_fnms(refined,product,splat(0x3F800001u));
    u128 half=spu_fm(product,splat(0x3F000000u));
    u128 result=spu_fma(residual,half,product);
    assert(result._u32[0]==0);
    for (int i=1;i<4;i++) assert(isfinite(result._f32[i]) && fabsf(result._f32[i]-sqrtf(x._f32[i]))<0.0001f);
    /* Extended range (2.11 EQ): exponent 255 is a finite SPU value. Products,
     * sums and compares use it as a number, saturate at +-0x7FFFFFFF and
     * truncate; the host's NaN/Inf reading must never reach a result. */
    same(spu_fm(splat(0xFFFFFFFFu), splat(0x3A800000u)), splat(0xFAFFFFFFu));   /* -max_ext * 2^-10 */
    same(spu_fm(splat(0x7F800000u), splat(0x40800000u)), splat(0x7FFFFFFFu));   /* 2^128 * 4 saturates */
    same(spu_fm(splat(0x7F000000u), splat(0x40800000u)), splat(0x7FFFFFFFu));   /* 2^127 * 4 = 2^129 saturates */
    same(spu_fm(splat(0x7F000000u), splat(0x40000000u)), splat(0x7F800000u));   /* 2^127 * 2 = 2^128 */
    same(spu_fa(splat(0x7F800000u), splat(0x3F800000u)), splat(0x7F800000u));   /* 2^128 + 1 truncates */
    same(spu_fs(splat(0xFFFFFFFFu), splat(0xFFFFFFFFu)), splat(0));
    same(spu_fma(splat(0xFFFFFFFFu), splat(0x00000000u), splat(0x3F800000u)), splat(0x3F800000u));
    same(spu_fnms(splat(0x7F800000u), splat(0x34000000u), splat(0x7F800000u)),  /* 2^128 - 2^128*2^-23 */
         splat(0x7F7FFFFEu));
    same(spu_fcgt(splat(0x7F800000u), splat(0x3F800000u)), splat(0xFFFFFFFFu));
    same(spu_fcgt(splat(0xFFFFFFFFu), splat(0xBF800000u)), splat(0));
    same(spu_fceq(splat(0x7FC00000u), splat(0x7FC00000u)), splat(0xFFFFFFFFu));
    /* Random lanes with an exponent-255 operand match an independent
     * double-precision SPU model (truncate, saturate, flush). */
    for (unsigned k=0;k<200000;k++) {
        u128 a,b;
        for (int i=0;i<4;i++) { a._u32[i]=random32(); b._u32[i]=random32(); if (k&1) a._u32[i]|=0x7F800000u; }
        const u128 r=spu_fm(a,b);
        for (int i=0;i<4;i++) {
            const uint32_t x=a._u32[i], y=b._u32[i];
            const int ex=(x>>23)&255, ey=(y>>23)&255;
            if (ex!=255 && ey!=255) continue;
            if (!ex || !ey) { assert(r._u32[i]==0); continue; }
            const double v=ldexp(1.0+(x&0x7FFFFF)/8388608.0,ex-127)*ldexp(1.0+(y&0x7FFFFF)/8388608.0,ey-127);
            const uint32_t s=(x^y)&0x80000000u;
            uint32_t want;
            if (v<0x1p-126) want=0; else if (v>=0x1p129) want=s|0x7FFFFFFFu;
            else { int e; const double m=frexp(v,&e)*2.0; want=s|((uint32_t)(e+126)<<23)|(uint32_t)((m-1.0)*8388608.0); }
            assert(r._u32[i]==want);
        }
    }
    puts("audio extended/zero: synthetic zero, interpolation, ordinary parity, sqrt and extended-range checks passed");
    return 0;
}
