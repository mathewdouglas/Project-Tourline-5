#include "spu_context.h"
#include <stdio.h>
#include <string.h>
#include <xmmintrin.h>
#include <pmmintrin.h>
#define D(n) u128 s_##n(u128,u128,u128); u128 v_##n(u128,u128,u128);
D(fa) D(fs) D(fm) D(fceq) D(fcgt) D(fma) D(fms) D(fnms)
typedef u128 (*F)(u128,u128,u128);
static uint64_t st=88172645463325252ull; static uint32_t rnd(void){ st^=st<<13; st^=st>>7; st^=st<<17; return (uint32_t)st; }
static const uint32_t edge[]={0,0x80000000,0x7F800000,0xFF800000,0x7FC00000,0xFFC00001,0x7F800001,1,0x80000001,0x007FFFFF,0x00800000,0x7F7FFFFF,0xFF7FFFFF,0x3F800000,0xBF800000};
static uint32_t pick(void){ uint32_t r=rnd(); return (r&7)==0 ? edge[rnd()%15] : rnd(); }
int main(void){
  F s[]={s_fa,s_fs,s_fm,s_fceq,s_fcgt,s_fma,s_fms,s_fnms}, v[]={v_fa,v_fs,v_fm,v_fceq,v_fcgt,v_fma,v_fms,v_fnms};
  const char* nm[]={"fa","fs","fm","fceq","fcgt","fma","fms","fnms"};
  unsigned long long bad[8]={0}, badnan[8]={0}, n=0;
  for (int mode=0; mode<2; mode++) {
    _mm_setcsr(mode ? (_mm_getcsr()|0x8040) : (_mm_getcsr()&~0x8040u));
    for (int it=0; it<4000000; it++) {
      u128 a,b,c; for(int i=0;i<4;i++){a._u32[i]=pick();b._u32[i]=pick();c._u32[i]=pick();}
      for (int k=0;k<8;k++){ u128 x=s[k](a,b,c), y=v[k](a,b,c); if (memcmp(&x,&y,16)) {
        int nan=0; for(int i=0;i<4;i++) if(x._u32[i]!=y._u32[i] && (x._u32[i]&0x7F800000)==0x7F800000 && (x._u32[i]&0x7FFFFF) && (y._u32[i]&0x7F800000)==0x7F800000 && (y._u32[i]&0x7FFFFF)) nan=1;
        if (nan) badnan[k]++; else bad[k]++; } }
      n++;
    }
  }
  int fail=0; for(int k=0;k<8;k++){ printf("%-5s differ %llu (NaN payload only %llu)\n", nm[k], bad[k], badnan[k]); fail|=bad[k]!=0; }
  printf("%llu vectors x 8 ops, FTZ/DAZ off and on\n", n); return fail;
}
