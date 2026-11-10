/* Host synchronization fixture: runtime functions are extracted unchanged.
 * A profiler-exit stub pauses at the actual pre-epilogue call boundary. */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sys_ppu_thread.h"
#define PPU_TLS __declspec(thread)
static PPU_TLS jmp_buf s_exit_jmp;
static PPU_TLS int s_exit_armed;
static CRITICAL_SECTION s_table_lock;
static int s_table_lock_init;
ppu_thread_info g_ppu_threads[PPU_THREAD_MAX];
ppu_thread_entry_fn g_ppu_thread_entry_trampoline;
static HANDLE gate_enter, gate_release, join_enter;
static volatile LONG hold_exit;
static unsigned char guest_memory[32];
uint8_t* vm_base = guest_memory;
#define status_bytes (guest_memory + 8)
void ppu_thread_note_block(uint64_t tid) { (void)tid; }
void ppu_resv_register(ppu_context* c) { (void)c; }
void ps3_thread_name(const char* fmt, ...) { (void)fmt; }
void ps3_tprof_thread(char kind, uint32_t tid) { (void)kind; (void)tid; }
void ps3_tprof_thread_exit(void) {
    if (InterlockedCompareExchange(&hold_exit,0,0)) {
        SetEvent(gate_enter);
        if (WaitForSingleObject(gate_release,5000)!=WAIT_OBJECT_0) abort();
    }
}
int ps3_intr_is_handler(unsigned long long tid) { (void)tid; return 0; }
int ps3_intr_wait(unsigned long long tid) { (void)tid; return 0; }
#include "runtime_join.inc"
static void require(int ok, const char* message) {
    if (!ok) { fprintf(stderr,"FAIL: %s\n",message); exit(1); }
}
static void guest_exit(ppu_context* c) { c->gpr[3]=0x12345678; sys_ppu_thread_exit(c); abort(); }
static DWORD WINAPI join_proc(void* unused) {
    (void)unused;
    ppu_context c; memset(&c,0,sizeof c); c.thread_id=1;c.gpr[3]=2;c.gpr[4]=8;
    SetEvent(join_enter);
    require(sys_ppu_thread_join(&c)==CELL_OK,"join success"); return 0;
}
int main(int argc,char** argv) {
    int legacy=argc>1 && !strcmp(argv[1],"legacy");
    table_lock(); table_unlock();
    gate_enter=CreateEventA(NULL,TRUE,FALSE,NULL);
    gate_release=CreateEventA(NULL,TRUE,FALSE,NULL);
    join_enter=CreateEventA(NULL,TRUE,FALSE,NULL);
    g_ppu_thread_entry_trampoline=guest_exit;
    ppu_thread_info* t=&g_ppu_threads[1];
    for(int cycle=0;cycle<128;cycle++) {
        memset(t,0,sizeof *t);t->ctx.thread_id=2;t->joinable=1;t->state=PPU_THREAD_STATE_RUNNING;
        t->finish_event=CreateEventA(NULL,TRUE,FALSE,NULL);
        ResetEvent(gate_enter);ResetEvent(gate_release);ResetEvent(join_enter);
        InterlockedExchange(&hold_exit,1);
        t->host_thread=CreateThread(NULL,0,ppu_host_thread_proc,t,CREATE_SUSPENDED,NULL);
        HANDLE host=NULL;
        require(DuplicateHandle(GetCurrentProcess(),t->host_thread,GetCurrentProcess(),&host,0,FALSE,DUPLICATE_SAME_ACCESS),"duplicate host handle");
        ResumeThread(t->host_thread);
        require(WaitForSingleObject(gate_enter,5000)==WAIT_OBJECT_0,"paused old epilogue");
        HANDLE join=CreateThread(NULL,0,join_proc,NULL,0,NULL);
        require(WaitForSingleObject(join_enter,5000)==WAIT_OBJECT_0,"join started");
        DWORD early=WaitForSingleObject(join,legacy?2000:50);
        require(early==(legacy?WAIT_OBJECT_0:WAIT_TIMEOUT),"join waits for host lifetime");
        require(t->state==(legacy?PPU_THREAD_STATE_FREE:PPU_THREAD_STATE_FINISHED),"slot stays owned until host exits");
        SetEvent(gate_release);
        require(WaitForSingleObject(host,5000)==WAIT_OBJECT_0,"host completed");
        require(WaitForSingleObject(join,5000)==WAIT_OBJECT_0,"join completed");
        require(t->state==(legacy?PPU_THREAD_STATE_FINISHED:PPU_THREAD_STATE_FREE),"old epilogue cannot resurrect slot");
        const unsigned char expected[8]={0,0,0,0,0x12,0x34,0x56,0x78};
        require(!memcmp(status_bytes,expected,8),"guest exit status big endian");
        CloseHandle(host);CloseHandle(join);
        if(legacy)break;
    }
    printf("%s: %s\n",legacy?"legacy":"fixed",legacy?"premature FREE then resurrected FINISHED reproduced":"128 delayed host exits joined without slot resurrection");
    CloseHandle(gate_enter);CloseHandle(gate_release);CloseHandle(join_enter);
    DeleteCriticalSection(&s_table_lock);return 0;
}

