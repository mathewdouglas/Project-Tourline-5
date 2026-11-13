/* Exercise the production DMA header. The Python driver builds this against
 * both the pre-change and patched header, and compares state and diagnostics.
 * Host hooks below are deterministic stand-ins, with observable call counts. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef DMA_HEADER
#define DMA_HEADER "spu_dma.h"
#endif
#include DMA_HEADER

uint8_t* vm_base;
uint8_t g_vm_page_bitmap[8192];
int g_spu_dma_log, g_cri_video_dma;
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
uint32_t g_barrier_sync_watch, g_ydkj_real_taskset_ea;
volatile unsigned g_wws_batch_gets;
static unsigned overlays, put_calls, guards, locks, notifications, wakes;
static int reserved;
int g_spu_ls_watch_n;
int pad_milestone_reached(const char* s) { return 1; }
unsigned long long ps3_ms_now(void) { return 1; }
unsigned* spu_ls_watch_list(int* n) { static unsigned w; *n = 0; return &w; }
void spu_overlay_note_get(spu_context* c, uint32_t ea, const uint8_t* p, uint32_t n) { overlays++; }
int spu_coh_is_reserved(uint32_t ea) { return reserved; }
void spu_coh_notify_write(uint32_t ea) { notifications++; }
void spu_coh_notify_write_except(uint32_t ea, const void* c) { notifications++; }
void spu_lockline_lock(void) { locks++; }
void spu_lockline_unlock(void) { assert(locks); }
void spu_lockguard_check(spu_context* c, uint32_t ea, const uint8_t* p, uint32_t n, const char* s) { guards++; }
static void put_hook(uint32_t ea, uint32_t n) { put_calls++; }
void (*g_spu_put_hook)(uint32_t, uint32_t) = put_hook;
unsigned spu_sn_defer_ticks(void) { return 0; }
void spu_sn_defer_arm(void* c, unsigned n) { abort(); }
void spu_ch_wake(spu_context* c) { wakes++; }
void spu_halt(spu_context* c) { fprintf(stderr, "unexpected halt\n"); abort(); }

static uint32_t seed = 0x12345678, hash = 2166136261u;
static uint32_t random_word(void) { seed = seed * 1664525u + 1013904223u; return seed; }
static void hash_bytes(const void* p, size_t n) {
    const uint8_t* b = p;
    while (n--) hash = (hash ^ *b++) * 16777619u;
}
static void write_channel(mfc_engine* m, spu_context* c, uint32_t ch, uint32_t v) {
#ifdef DMA_FAST_TEST
    if (spu_dma_fast() && mfc_context_write(c, ch, v)) return;
#endif
    mfc_channel_write(m, c, ch, v);
}
static void list_element(uint8_t* p, uint32_t flags, uint32_t ea) {
    uint32_t a = __builtin_bswap32(flags), b = __builtin_bswap32(ea);
    memcpy(p, &a, 4); memcpy(p + 4, &b, 4);
}
int main(int argc, char** argv) {
#ifdef DMA_FAST_TEST
    if (argc > 1 && !strcmp(argv[1], "gate")) {
        printf("fast=%d diag=%d\n", spu_dma_fast(), spu_dma_diag());
        return 0;
    }
#endif
    static spu_context ctx;
    ctx.ls = ctx.ls_store;
    ctx.image_id = 0; ctx.pc = 0x100;
    vm_base = VirtualAlloc(NULL, 0x40000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    assert(vm_base);
    /* Leave the page bitmap empty: exercise real demand-page validation. */
    for (unsigned i = 0; i < 0x40000; i++) vm_base[i] = (uint8_t)random_word();
    for (unsigned i = 0; i < SPU_LS_SIZE; i++) ctx.ls[i] = (uint8_t)random_word();
    mfc_engine engine;
    mfc_engine_init(&engine);
    const uint32_t chs[] = {MFC_LSA, MFC_EAH, MFC_EAL, MFC_Size, MFC_TagID, MFC_WrTagMask};
    for (unsigned i = 0; i < 5000; i++) {
        write_channel(&engine, &ctx, chs[i % 6], random_word());
        hash_bytes(&ctx.mfc_lsa, sizeof ctx.mfc_lsa);
        hash_bytes(&ctx.mfc_eah, sizeof ctx.mfc_eah);
        hash_bytes(&ctx.mfc_eal, sizeof ctx.mfc_eal);
        hash_bytes(&ctx.mfc_size, sizeof ctx.mfc_size);
        hash_bytes(&ctx.mfc_tag, sizeof ctx.mfc_tag);
        hash_bytes(&ctx.mfc_tag_mask, sizeof ctx.mfc_tag_mask);
    }
    for (unsigned i = 0; i < 300; i++) {
        unsigned n = 16u << (i % 11), ea = 0x10000 + (i % 8) * 0x1000;
        unsigned ls = 0x10000 + (i % 4) * 0x4000;
        reserved = i & 1;
        write_channel(&engine, &ctx, MFC_LSA, ls);
        write_channel(&engine, &ctx, MFC_EAH, 0);
        write_channel(&engine, &ctx, MFC_EAL, ea);
        write_channel(&engine, &ctx, MFC_Size, n);
        write_channel(&engine, &ctx, MFC_TagID, i & 31);
        write_channel(&engine, &ctx, MFC_Cmd, i & 2 ? MFC_GET_CMD : MFC_PUT_CMD);
        write_channel(&engine, &ctx, MFC_WrTagMask, 1u << (i & 31));
        for (unsigned mode = 0; mode < 4; mode++) {
            write_channel(&engine, &ctx, MFC_WrTagUpdate, mode);
            hash_bytes(&ctx.mfc_tag_status, sizeof ctx.mfc_tag_status);
        }
    }
    /* Invalid transfers must still be rejected before any copy or hook. */
    unsigned before_get = overlays, before_put = put_calls;
    assert(mfc_do_transfer(&ctx, 0x1000, 0x10000, 0, MFC_GET_CMD) == 0);
    assert(mfc_do_transfer(&ctx, 0x1000, 0x10000, 0x4010, MFC_GET_CMD) == 0);
    assert(mfc_do_transfer(&ctx, 0x1001, 0x10000, 16, MFC_GET_CMD) == 0);
    assert(mfc_do_transfer(&ctx, 0x1000, 0, 16, MFC_PUT_CMD) == 0);
    assert(overlays == before_get && put_calls == before_put);
    /* Stream a stalled list, consume read-and-clear status, then resume. */
    reserved = 0;
    list_element(ctx.ls + 0x3000, 0x80000010u, 0x10000);
    list_element(ctx.ls + 0x3008, 0x10, 0x10010);
    ctx.mfc_lsa = 0x4000; ctx.mfc_eal = 0x3000;
    ctx.mfc_eah = 0; ctx.mfc_size = 16; ctx.mfc_tag = 7;
    write_channel(&engine, &ctx, MFC_Cmd, MFC_GETL_CMD);
    assert(ctx.list_stall_mask == (1u << 7));
    assert(mfc_channel_read(NULL, &ctx, MFC_RdListStallStat) == (1u << 7));
    assert(mfc_channel_read(NULL, &ctx, MFC_RdListStallStat) == 0);
    write_channel(&engine, &ctx, MFC_WrListStallAck, 7);
    assert(!ctx.list_stall_mask);
    assert(!memcmp(ctx.ls + 0x4000, vm_base + 0x10000, 32));
    ctx.atomic_stat = 1;
    assert(mfc_channel_read(NULL, &ctx, MFC_RdAtomicStat) == 1);
    assert(mfc_channel_read(NULL, &ctx, MFC_RdTagMask) == ctx.mfc_tag_mask);
    hash_bytes(vm_base, 0x40000); hash_bytes(ctx.ls, SPU_LS_SIZE);
    hash_bytes(&engine, sizeof engine);
    printf("state=%08X overlay=%u put=%u guard=%u lock=%u notify=%u wake=%u\n",
           hash, overlays, put_calls, guards, locks, notifications, wakes);
    assert(overlays && put_calls && guards && locks && notifications && wakes);
    VirtualFree(vm_base, 0, MEM_RELEASE);
    return 0;
}
