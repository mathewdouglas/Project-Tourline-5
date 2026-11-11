#include "spu_helpers.h"
#include <assert.h>
#include "../../src/gt5p_spu_native.c"
#include "clear.inc"
int g_spu_ls_watch_n;
void spu_ls_watch_slow(uint32_t a, int b, const uint8_t* p, uint32_t pc, uint32_t lr) { abort(); }
static uint32_t seed=0x20261111;
static uint32_t rnd(void) { seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed; }
static spu_context ref, opt;
int main(void) {
    s_stats=0;
    for(unsigned t=0;t<1200;t++) {
        memset(&ref,0,sizeof ref);ref.ls=ref.ls_store;
        for(unsigned i=0;i<SPU_LS_SIZE;i+=4) { uint32_t v=rnd();memcpy(ref.ls+i,&v,4); }
        for(unsigned r=0;r<128;r++) for(unsigned k=0;k<4;k++)ref.gpr[r]._u32[k]=rnd();
        unsigned start=(rnd()%1024)*4, words=1+rnd()%2048;
        for(unsigned k=0;k<4;k++) {
            ref.gpr[15]._u32[k]=start+(k?rnd():0);
            ref.gpr[6]._u32[k]=start+words*4;
            ref.gpr[83]._u32[k]=rnd();
        }
        /* Include LS-end wrap, unaligned bases and both arithmetic wraps. */
        ref.gpr[83]._u32[0]=t%2?rnd():SPU_LS_SIZE-start-32;
        ref.gpr[7]=spu_il(t%7==0?42:0); /* nonzero source must fall back */
        opt=ref;opt.ls=opt.ls_store;
        s_mode=0;lifted_clear(&ref);
        s_mode=1;gt5p_spu_clear_words(&opt);lifted_clear(&opt);
        assert(!memcmp(ref.gpr,opt.gpr,sizeof ref.gpr));
        assert(!memcmp(ref.ls,opt.ls,SPU_LS_SIZE));
    }
    /* Check mode must leave the state unchanged. */
    memset(&opt,0,sizeof opt);opt.ls=opt.ls_store;opt.gpr[6]=spu_il(1280);ref=opt;
    s_mode=2;gt5p_spu_clear_words(&opt);assert(!memcmp(&ref,&opt,sizeof opt));
    puts("GT5 table clear: 1200 full-LS/register parity cases and check-mode retention passed");
}
