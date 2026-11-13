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
    puts("audio extended/zero: synthetic zero, interpolation, ordinary parity and sqrt checks passed");
    return 0;
}
