/* Exercise the production helper's pause, yield, timer and rollback tiers. */
#include "../../src/gt5p_spin.cpp"
#include <assert.h>
#include <stdio.h>
extern "C" volatile int g_tl_on=1;
static unsigned sleeps;
extern "C" uint64_t ps3_tl_us(void) {
    LARGE_INTEGER q,f;QueryPerformanceCounter(&q);QueryPerformanceFrequency(&f);
    return (uint64_t)(q.QuadPart*1000000/f.QuadPart);
}
extern "C" void ps3_tl_rec(char kind,uint32_t count,uint32_t elapsed) {
    assert(kind=='Q' && count==64 && elapsed>0);++sleeps;
}
int main(int argc,char** argv) {
    assert(argc==2);
    uint32_t n=0;gt5p_spin_pause(&n);assert(n==1 && sleeps==0);
    n=32;gt5p_spin_pause(&n);assert(n==33 && sleeps==0);
    n=64;gt5p_spin_pause(&n);assert(n==65 && sleeps==(unsigned)atoi(argv[1]));
    puts("production spin helper: pause/yield/timer tiers and rollback passed");
}
