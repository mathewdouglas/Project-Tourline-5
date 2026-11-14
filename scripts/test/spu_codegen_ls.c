#define SPU_LS_NO_PROBES
#include "spu_helpers.h"
#include <assert.h>
#include <stdio.h>
static spu_context a,b;
static uint32_t seed=0x20261114;
static uint32_t rnd(void) { seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed; }
int main(void) {
    a.ls=a.ls_store;b.ls=b.ls_store;
    for(unsigned i=0;i<SPU_LS_SIZE;i++) a.ls[i]=b.ls[i]=(uint8_t)rnd();
    for(unsigned t=0;t<100000;t++) {
        uint32_t addr=rnd();u128 x=spu_ls_read128(&a,addr),y=spu_codegen_lq(b.ls,addr);
        assert(!memcmp(&x,&y,16));
        for(unsigned i=0;i<4;i++) x._u32[i]=rnd();
        spu_ls_write128(&a,addr,x);spu_codegen_sq(b.ls,addr,x);
        assert(!memcmp(a.ls+(addr&(SPU_LS_MASK&~15u)),b.ls+(addr&(SPU_LS_MASK&~15u)),16));
    }
    assert(!memcmp(a.ls,b.ls,SPU_LS_SIZE));
    puts("Explicit LS base: 100000 random loads/stores, mask/alignment/endian results exact");
}
