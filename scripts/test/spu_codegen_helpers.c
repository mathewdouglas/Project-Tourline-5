#define SPU_CODEGEN_HELPERS
#include "spu_helpers.h"
#include <assert.h>
#include <stdio.h>
static uint32_t seed=0x20261114;
static uint32_t rnd(void) { seed^=seed<<13; seed^=seed>>17; seed^=seed<<5; return seed; }
static unsigned cases;
static void eq(u128 a,u128 b) { assert(!memcmp(&a,&b,16)); cases++; }
#define BIN(N) eq(spu_##N(a,b),(spu_##N)(a,b))
#define IMM(N) eq(spu_##N(a,i),(spu_##N)(a,i))
int main(void) {
    for(unsigned t=0;t<200000;t++) {
        u128 a,b; for(int k=0;k<4;k++) { a._u32[k]=rnd(); b._u32[k]=rnd(); }
        BIN(nand); BIN(nor); BIN(andc); BIN(orc);
        BIN(cbx); BIN(chx); BIN(cwx); BIN(cdx);
        int i=(int)(t&255)-128;
        IMM(cbd); IMM(chd); IMM(cwd); IMM(cdd);
        IMM(shli); IMM(shlhi); IMM(rotmi); IMM(rotmhi);
        IMM(shlqbii); IMM(rotqbii); IMM(rotqmbii);
    }
    printf("SIMD helpers: %u exact cases (random vectors, all encoded immediate counts)\n",cases);
}
