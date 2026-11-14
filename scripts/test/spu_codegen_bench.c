/* Integer helper throughput. Thread cycle accounting excludes descheduled
 * time, but this is still a synthetic workload, not an in-game FPS estimate. */
#define SPU_CODEGEN_HELPERS
#include "spu_helpers.h"
#include <assert.h>
#include <stdio.h>
#include <windows.h>
static volatile uint32_t input_seed=0x12345678;
static volatile uint64_t sink;
#define BODY(BIN,IMM) \
    u128 a=spu_il(input_seed),b=spu_il(input_seed^0x3456); \
    for(unsigned i=0;i<n;i++) { \
        u128 c=IMM(cwd,a,(int)i); a=spu_shufb(a,b,c); \
        b=BIN(andc,b,c); a=BIN(nand,a,b); \
        a=spu_xor(a,IMM(cbd,b,(int)(i>>2))); \
        b=spu_a(b,IMM(shli,a,(int)(i&7))); \
        a=IMM(rotqbii,a,(int)(i&7)); \
    } \
    return a._u64[0]^a._u64[1]^b._u64[0]^b._u64[1];
#define OLD_BIN(N,A,B) (spu_##N)(A,B)
#define NEW_BIN(N,A,B) spu_##N(A,B)
__declspec(noinline) static uint64_t old_kernel(unsigned n) { BODY(OLD_BIN,OLD_BIN) }
__declspec(noinline) static uint64_t new_kernel(unsigned n) { BODY(NEW_BIN,NEW_BIN) }
static int cmp(const void* a,const void* b) { double x=*(const double*)a,y=*(const double*)b; return (x>y)-(x<y); }
int main(void) {
    double old[16],newv[16];
    for(unsigned r=0;r<16;r++) for(unsigned s=0;s<2;s++) {
        unsigned which=s^(r&1); unsigned long long begin,end;
        QueryThreadCycleTime(GetCurrentThread(),&begin);
        uint64_t hash=which?new_kernel(200000):old_kernel(200000);
        QueryThreadCycleTime(GetCurrentThread(),&end);
        sink=hash; assert(hash==old_kernel(200000));
        (which?newv:old)[r]=(double)(end-begin);
    }
    qsort(old,16,sizeof(double),cmp);qsort(newv,16,sizeof(double),cmp);
    printf("integer mix median thread cycles: old %.0f, SIMD %.0f; ratio %.4f; digest %016llX\n",old[8],newv[8],old[8]/newv[8],sink);
}
