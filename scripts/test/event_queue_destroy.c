/* Production event-queue code, synthetic workers, no guest/game execution. */
#include "sys_event.c"
#include <process.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); ExitProcess(1); } } while (0)
uint8_t* vm_base;
void ydkj_release_pending_threads(void) {}
void ppu_dump_guest_stack(ppu_context* ctx, const char* tag) { (void)ctx; (void)tag; }
size_t spu_elf_image_size(const uint8_t* image, size_t size) { (void)image; (void)size; return 0; }
int spu_workload_dispatch_async(const uint8_t* image, uint32_t size, uint32_t ea)
{ (void)image; (void)size; (void)ea; return 0; }
int spu_dispatch_frame_by_queue(uint32_t queue, uint32_t ea) { (void)queue; (void)ea; return 0; }
uint32_t g_spu_pending_evt[3];
int g_spu_pending_evt_valid;
/* Event-flag functions share the production file but are outside this test. */
int64_t lv2_usec_deadline(uint64_t usec) { (void)usec; abort(); }
int lv2_deadline_passed(int64_t deadline) { (void)deadline; abort(); }

typedef struct worker {
    uint32_t id;
    uint64_t timeout;
    int64_t result;
    uint64_t data;
} worker;

static unsigned __stdcall receive(void* arg)
{
    worker* w = arg;
    ppu_context ctx = {0};
    ctx.gpr[3] = w->id;
    ctx.gpr[5] = w->timeout;
    w->result = sys_event_queue_receive(&ctx);
    w->data = ctx.gpr[5];
    return 0;
}

static int64_t destroy(uint32_t id, int mode)
{
    ppu_context ctx = {0};
    ctx.gpr[3] = id;
    ctx.gpr[4] = mode;
    return sys_event_queue_destroy(&ctx);
}

static void await_receivers(uint32_t id, unsigned expected)
{
    ULONGLONG until = GetTickCount64() + 2000;
    for (;;) {
        evt_table_lock();
        sys_event_queue_info* q = &g_sys_event_queues[id - 1];
        EnterCriticalSection(&q->lock);
        unsigned n = q->waiters;
        LeaveCriticalSection(&q->lock);
        evt_table_unlock();
        if (n == expected) return;
        CHECK(GetTickCount64() < until);
        Sleep(1);
    }
}

static void join(HANDLE h)
{
    CHECK(WaitForSingleObject(h, 2000) == WAIT_OBJECT_0);
    CloseHandle(h);
}

int main(void)
{
    _putenv_s("PS3_VERBOSE", "0");
    /* Warm diagnostics before concurrent first calls. */
    uint32_t id = sys_event_queue_create_direct(0, 4);
    CHECK(id);
    worker poll = {id, 1, 0, 0};
    receive(&poll);
    CHECK((uint32_t)poll.result == CELL_ETIMEDOUT);
    CHECK((uint32_t)destroy(id, 2) == CELL_EINVAL);

    /* Normal destroy refuses receivers; forced destroy releases ALL of them,
     * including a long timed wait, with cancellation rather than fake events. */
    worker workers[4] = {{id, 0}, {id, 0}, {id, 0}, {id, 60000000}};
    HANDLE threads[4];
    for (unsigned i = 0; i < 4; ++i)
    {
        threads[i] = (HANDLE)_beginthreadex(NULL, 0, receive, &workers[i], 0, NULL);
        CHECK(threads[i]);
    }
    await_receivers(id, 4);
    CHECK((uint32_t)destroy(id, 0) == CELL_EBUSY);
    CHECK(destroy(id, 1) == CELL_OK);
    for (unsigned i = 0; i < 4; ++i) {
        join(threads[i]);
        CHECK((uint32_t)workers[i].result == CELL_ECANCELED);
    }
    CHECK(sys_event_queue_push_by_id(id, 0, 1, 2, 3) == -1);
    CHECK((uint32_t)destroy(id, 1) == CELL_ESRCH);

    /* Hold an operation across destroy/recreate. Its slot and primitives must
     * remain reserved, then become reusable after the final reference leaves. */
    id = sys_event_queue_create_direct(0, 4);
    sys_event_queue_info* retained = event_queue_acquire(id);
    CHECK(retained);
    CHECK(destroy(id, 0) == CELL_OK); /* References alone are not waiters. */
    uint32_t other = sys_event_queue_create_direct(0, 4);
    CHECK(other && other != id);
    CHECK(event_queue_push_impl(retained, &(sys_event_t){0, 123, 0, 0}) == -1);
    event_queue_release(retained);
    CHECK(sys_event_queue_create_direct(0, 4) == id);
    CHECK(sys_event_queue_push_by_id(id, 9, 123, 2, 3) == 0);
    worker valid = {id, 1};
    receive(&valid);
    CHECK(valid.result == CELL_OK && valid.data == 123);
    CHECK(destroy(id, 0) == CELL_OK);
    CHECK(destroy(other, 0) == CELL_OK);

    /* Repeated waiter destruction/reuse catches stale synchronization state. */
    for (unsigned i = 0; i < 100; ++i) {
        id = sys_event_queue_create_direct(0, 4);
        worker w = {id, 0};
        HANDLE h = (HANDLE)_beginthreadex(NULL, 0, receive, &w, 0, NULL);
        CHECK(h);
        await_receivers(id, 1);
        CHECK(destroy(id, 1) == CELL_OK);
        other = sys_event_queue_create_direct(0, 4);
        CHECK(other);
        CHECK(sys_event_queue_push_by_id(other, 0, 456, 0, 0) == 0);
        join(h);
        CHECK((uint32_t)w.result == CELL_ECANCELED);
        valid = (worker){other, 1};
        receive(&valid);
        CHECK(valid.result == CELL_OK && valid.data == 456);
        CHECK(destroy(other, 0) == CELL_OK);
    }
    puts("event queue destroy: cancellation, normal/force modes, slot lifetime and reuse passed");
    return 0;
}
