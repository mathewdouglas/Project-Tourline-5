/* Synthetic inputs to the private, actual GT5 coefficient constructor.
 * The extracted lifted source is generated into scratch by the Python driver. */
#include "gt5p_spu_audio_math.h"
#include <assert.h>
#include <stdio.h>

volatile int g_spu_chain_back=1;
unsigned g_spu_stack_reset_n;
int g_spu_ls_watch_n;
SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*);
void spu_indirect_branch(spu_context* ctx) { (void)ctx; abort(); }
void gt5p_j9804ABA8_spu_func_000004E8(spu_context*);
static spu_context ctx;

static void calculate(float hz, double out[5])
{
    memset(&ctx,0,sizeof ctx); ctx.ls=ctx.ls_store; ctx.host_depth=1;
    ctx.gpr[0]._u32[0]=0x3FFF0;
    ctx.gpr[3]._u32[0]=0x8000;
    for (int i=0;i<4;i++) {
        ctx.gpr[4]._u32[i]=1;
        ctx.gpr[5]._f32[i]=hz;
        ctx.gpr[6]._f32[i]=0.55f;
        ctx.gpr[7]._f32[i]=72.0f;
    }
    gt5p_j9804ABA8_spu_func_000004E8(&ctx);
    assert(ctx.pc==0x3FFF0 && !g_spu_trampoline_fn);
    for (int i=0;i<5;i++) {
        const unsigned a=0x8000+4*i;
        uint32_t bits=((uint32_t)ctx.ls[a]<<24)|((uint32_t)ctx.ls[a+1]<<16)|
                      ((uint32_t)ctx.ls[a+2]<<8)|ctx.ls[a+3];
        float f; memcpy(&f,&bits,4); assert(isfinite(f)); out[i]=f;
    }
}

static double tail(const double c[5])
{
    double y1=c[0],y2=0;
    for (int n=1;n<1000000;n++) {
        const double y=(n==1?c[1]:n==2?c[2]:0)-c[3]*y1-c[4]*y2;
        y2=y1; y1=y;
    }
    return fabs(y1);
}

int main(int argc,char** argv)
{
    (void)argv;
    const int rollback=argc>1;
    const unsigned original=_mm_getcsr();
    _mm_setcsr((original&~0x6000u)|0x8040u);
    double nearest[5],scoped[5];
    calculate(1.0f,nearest);
    assert(1+nearest[3]+nearest[4]<0); /* captured unstable denominator */
    const unsigned before=_mm_getcsr();
    const unsigned saved=gt5p_spu_audio_round_enter();
    assert((_mm_getcsr()&0x6000u)==(rollback?0:0x6000u));
    calculate(1.0f,scoped);
    gt5p_spu_audio_round_leave(saved);
    assert(_mm_getcsr()==before);
    if (rollback) assert(!memcmp(nearest,scoped,sizeof nearest));
    else {
        assert(1+scoped[3]+scoped[4]>0 && fabs(scoped[4])<1);
        assert(tail(nearest)>1e20 && tail(scoped)<1e-6);
    }
    for (unsigned mode=0;mode<4;mode++) {
        const unsigned state=(original&~0x6000u)|(mode<<13);
        _mm_setcsr(state);
        unsigned outer=gt5p_spu_audio_round_enter(),inside=_mm_getcsr();
        unsigned inner=gt5p_spu_audio_round_enter();
        gt5p_spu_audio_round_leave(inner); assert(_mm_getcsr()==inside);
        gt5p_spu_audio_round_leave(outer); assert(_mm_getcsr()==state);
    }
    _mm_setcsr(original);
    printf("audio coefficients: RNE margin %.12g, scoped %.12g; %s, MXCSR restored\n",
           1+nearest[3]+nearest[4],1+scoped[3]+scoped[4],rollback?"rollback matched":"million-sample stability passed");
    return 0;
}
