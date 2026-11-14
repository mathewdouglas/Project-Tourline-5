#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
static unsigned long long hash_calls;
static uint64_t test_hash(const void* p, size_t n) {
    const uint8_t* b=p; uint64_t h=1469598103934665603ULL;
    InterlockedIncrement64((volatile LONG64*)&hash_calls);
    for(size_t i=0;i<n;i++) h=(h^b[i])*1099511628211ULL;
    return h;
}
#define SPU_JOB_FP_HASH test_hash
#include "spu_job_fingerprint.h"
static uint8_t images[80][4096];
static DWORD WINAPI worker(void* arg) {
    unsigned offset=(unsigned)(uintptr_t)arg;
    for(unsigned pass=0;pass<30;pass++) for(unsigned i=0;i<80;i++) {
        unsigned k=(i+offset)%80;
        assert(spu_job_fingerprint(images[k],4096)==test_hash(images[k],4096));
    }
    return 0;
}
int main(int argc,char** argv) {
    assert(argc>1);
    unsigned slots=(unsigned)atoi(argv[1]);
    _putenv_s("SPU_JOB_FP_SLOTS",argv[1]);
    int verify=argc>2;
    if(verify) _putenv_s("SPU_JOB_FP_VERIFY","1");
    for(unsigned i=0;i<80;i++) memset(images[i],(int)i+1,4096);
    uint64_t f=spu_job_fingerprint(images[0],4096);
    assert(f==spu_job_fingerprint(images[0],4096));
    images[0][0]^=1; assert(f!=spu_job_fingerprint(images[0],4096));
    f=spu_job_fingerprint(images[0],4096);
    images[0][1]^=1; /* deliberately outside the sparse sample */
    uint64_t changed=spu_job_fingerprint(images[0],4096);
    assert(verify ? changed!=f : changed==f);
    images[0][1]^=1;
    /* Distinct workers must never share partially published memo entries. */
    HANDLE threads[4];
    for(unsigned i=0;i<4;i++) threads[i]=CreateThread(NULL,0,worker,(void*)(uintptr_t)i,0,NULL);
    assert(WaitForMultipleObjects(4,threads,TRUE,INFINITE)==WAIT_OBJECT_0);
    for(unsigned i=0;i<4;i++) CloseHandle(threads[i]);
    printf("fingerprint slots=%u: changed image, wraparound, 9600 concurrent lookups agree\n",slots);
    return 0;
}
