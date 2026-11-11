#include "spu_context.h"
#include <assert.h>
#include <intrin.h>
void reference_4720(spu_context*), reference_4C80(spu_context*);
void fast_4720(spu_context*), fast_4C80(spu_context*);
int g_spu_ls_watch_n;
void spu_ls_watch_slow(uint32_t a, int b, const uint8_t* p, uint32_t pc, uint32_t lr) { abort(); }
static __attribute__((target("xsave"),noinline)) uint64_t xcr0(void){return _xgetbv(0);}
static uint32_t seed=0x20261111;
static uint32_t rnd(void){seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed;}
static float signal(void){return ((int)(rnd()%2001)-1000)/4096.0f;}
static spu_context input,ref,opt;
static u128 splat(uint32_t v){u128 r;for(unsigned k=0;k<4;k++)r._u32[k]=v;return r;}
int main(void) {
    int cpu[4];__cpuidex(cpu,1,0);unsigned req=(1u<<12)|(1u<<27)|(1u<<28);
    if(((unsigned)cpu[2]&req)!=req||(xcr0()&6)!=6){puts("GT5 mixer: FMA unavailable, skipped");return 0;}
    unsigned old=_mm_getcsr();
    for(unsigned ftz=0;ftz<2;ftz++) {
        _mm_setcsr(ftz?old|0x8040:old&~0x8040);
        for(unsigned t=0;t<128;t++) {
            memset(&input,0,sizeof input);input.ls=input.ls_store;
            for(unsigned a=0;a<SPU_LS_SIZE;a+=4) {
                float f=signal();uint32_t w;memcpy(&w,&f,4);w=__builtin_bswap32(w);memcpy(input.ls+a,&w,4);
            }
            for(unsigned r=0;r<128;r++)for(unsigned k=0;k<4;k++)input.gpr[r]._f32[k]=signal();
            unsigned start=t%2?0x1000:SPU_LS_SIZE-16, count=1+rnd()%512;
            input.gpr[7]=splat(start);input.gpr[22]=splat(start+count*4);
            input.gpr[14]=splat(0);input.gpr[17]=splat(0x8000);
            for(unsigned r=23;r<=27;r++)input.gpr[r]=splat(0x20000+r*16);
            for(unsigned kernel=0;kernel<2;kernel++) {
                ref=input;ref.ls=ref.ls_store;opt=input;opt.ls=opt.ls_store;
                (kernel?reference_4C80:reference_4720)(&ref);
                (kernel?fast_4C80:fast_4720)(&opt);
                assert(ref.pc==opt.pc);
                assert(!memcmp(ref.gpr,opt.gpr,sizeof ref.gpr));
                assert(!memcmp(ref.ls,opt.ls,SPU_LS_SIZE));
            }
        }
    }
    _mm_setcsr(old);
    puts("GT5 mixer: 512 hot-loop full-LS/register comparisons match scalar fmaf (FTZ/DAZ on and off)");
}
