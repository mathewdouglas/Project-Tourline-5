/* Harness for scripts/test/volmap_fs.py: drives the runtime's real cellFs.c
 * and volmap.c against a synthetic install (guest memory is a host buffer). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "cellFs.h"
#include "ps3emu/endian.h"

uint8_t* vm_base;

/* Symbols cellFs.c refers to that belong to other runtime parts. */
void pad_milestone_seen(const char* p) { (void)p; }
unsigned int ppu_prof_resolve_host(void* p) { (void)p; return 0; }
unsigned long long ps3_ms_now(void) { return 0; }
void ps3_vfs_ps3game_fallback(char* path, size_t cap) { (void)path; (void)cap; }
/* Guest-memory coherence hooks of ppu_memory.h's inline accessors: no other
 * guest threads or SPUs exist here. */
void* g_active_ctx;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}

#define G_PATH  0x1000u
#define G_FD    0x2000u
#define G_N     0x2010u
#define G_POS   0x2020u
#define G_STAT  0x3000u
#define G_BUF   0x100000u

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static const char* gp(const char* s) { strcpy((char*)vm_base + G_PATH, s); return (const char*)(uintptr_t)G_PATH; }
static s32 fd_of(void) { return (s32)ps3_bswap32(*(u32*)(vm_base + G_FD)); }
static u64 be64(u32 ea) { return ps3_bswap64(*(u64*)(vm_base + ea)); }

static unsigned char* slurp(const char* path, long* n)
{
    FILE* f = fopen(path, "rb");
    if (!f) { *n = -1; return NULL; }
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char* b = malloc(*n + 1);
    if (fread(b, 1, *n, f) != (size_t)*n) *n = -1;
    fclose(f);
    return b;
}

static u64 stat_size(const char* guest, s32* rc)
{
    *rc = cellFsStat(gp(guest), (CellFsStat*)(uintptr_t)G_STAT);
    return ((CellFsStat*)(vm_base + G_STAT))->st_size == 0 ? 0 : ps3_bswap64(((CellFsStat*)(vm_base + G_STAT))->st_size);
}

static u64 rd(s32 fd, u64 n, s32* rc)
{
    *rc = cellFsRead(fd, (void*)(uintptr_t)G_BUF, n, (u64*)(uintptr_t)G_N);
    return be64(G_N);
}

static s64 seek(s32 fd, s64 off, s32 whence)
{
    s32 rc = cellFsLseek(fd, off, whence, (u64*)(uintptr_t)G_POS);
    return rc == CELL_OK ? (s64)be64(G_POS) : -1;
}

int main(int argc, char** argv)
{
    if (argc < 3) return 2;
    const char* root = argv[1];
    const char* mode = argv[2];
    char p[1024];
    vm_base = calloc(1, 64u << 20);
    cellfs_set_root_path(root);
    snprintf(p, sizeof p, "%s/disc/PS3_GAME/", root);
    cellfs_add_path_mapping("/dev_bdvd/PS3_GAME/", p);
    const char* A1 = "/dev_hdd0/game/BCUS98114/USRDIR/PDIPFS/A/1";
    const char* A2 = "/dev_hdd0/game/BCUS98114/USRDIR/PDIPFS/A/2";
    const char* B3 = "/dev_hdd0/game/BCUS98114/USRDIR/PDIPFS/B/3";
    long n1, n2;
    snprintf(p, sizeof p, "%s/expected/A1", root); unsigned char* e1 = slurp(p, &n1);
    snprintf(p, sizeof p, "%s/expected/A2", root); unsigned char* e2 = slurp(p, &n2);
    CHECK(e1 && e2, "expected files missing");
    if (!e1 || !e2) return 2;
    s32 rc;

    if (!strcmp(mode, "serve")) {
        CHECK(stat_size(A1, &rc) == (u64)n1 && rc == CELL_OK, "stat A1 size %llu", (unsigned long long)stat_size(A1, &rc));
        CHECK(stat_size(B3, &rc) == 6000 && rc == CELL_OK, "stat of an ordinary file");

        rc = cellFsOpen(gp(A1), CELL_FS_O_RDONLY, (CellFsFd*)(uintptr_t)G_FD, NULL, 0);
        CHECK(rc == CELL_OK, "open A1 read-only rc %X", rc);
        s32 fd = fd_of();
        u64 got = rd(fd, 4096, &rc);
        CHECK(got == 4096 && !memcmp(vm_base + G_BUF, e1, 4096), "first 4096 bytes");
        CHECK(seek(fd, 100, CELL_FS_SEEK_CUR) == 4196, "SEEK_CUR");
        got = rd(fd, 8, &rc);
        CHECK(got == 8 && !memcmp(vm_base + G_BUF, e1 + 4196, 8), "read after SEEK_CUR");
        CHECK(seek(fd, -10, CELL_FS_SEEK_END) == n1 - 10, "SEEK_END");
        got = rd(fd, 100, &rc);
        CHECK(got == 10 && !memcmp(vm_base + G_BUF, e1 + n1 - 10, 10), "read clamped at the range's end: %llu", (unsigned long long)got);
        CHECK(rd(fd, 100, &rc) == 0, "read at end returns 0");
        CHECK(seek(fd, n1 + 50, CELL_FS_SEEK_SET) == n1 + 50 && rd(fd, 10, &rc) == 0, "read past end returns 0");
        CHECK(seek(fd, -1, CELL_FS_SEEK_SET) == -1, "negative seek refused");
        CHECK(seek(fd, 0, CELL_FS_SEEK_SET) == 0, "SEEK_SET 0");
        got = rd(fd, 1 << 20, &rc);
        CHECK(got == (u64)n1 && !memcmp(vm_base + G_BUF, e1, n1), "whole file");
        rc = cellFsFstat(fd, (CellFsStat*)(uintptr_t)G_STAT);
        CHECK(rc == CELL_OK && ps3_bswap64(((CellFsStat*)(vm_base + G_STAT))->st_size) == (u64)n1, "fstat size");
        CHECK(cellFsWrite(fd, (void*)(uintptr_t)G_BUF, 4, (u64*)(uintptr_t)G_N) == CELL_FS_ERROR_EBADF, "write refused");
        CHECK(cellFsFtruncate(fd, 0) == CELL_FS_ERROR_EBADF, "ftruncate refused");
        CHECK(cellFsClose(fd) == CELL_OK, "close");

        /* O_CREAT|O_EXCL on a record: it exists. */
        rc = cellFsOpen(gp(A1), CELL_FS_O_RDWR | CELL_FS_O_CREAT | CELL_FS_O_EXCL, (CellFsFd*)(uintptr_t)G_FD, NULL, 0);
        CHECK(rc == (s32)CELL_EEXIST, "O_EXCL on a record rc %X", rc);

        /* Opening for writing turns the record back into the file. */
        rc = cellFsOpen(gp(A2), CELL_FS_O_RDWR, (CellFsFd*)(uintptr_t)G_FD, NULL, 0);
        CHECK(rc == CELL_OK, "open A2 read-write rc %X", rc);
        fd = fd_of();
        got = rd(fd, 1 << 20, &rc);
        CHECK(got == (u64)n2 && !memcmp(vm_base + G_BUF, e2, n2), "A2 after copy on write");
        cellFsClose(fd);
        long m; unsigned char* d;
        snprintf(p, sizeof p, "%s/gamedata/dev_hdd0/game/BCUS98114/USRDIR/PDIPFS/A/2", root);
        d = slurp(p, &m);
        CHECK(m == n2 && d && !memcmp(d, e2, n2), "A2 on disk is the real file");
        free(d);

        /* Truncate by path keeps the start of the contents. */
        CHECK(cellFsTruncate(gp(A1), 50) == CELL_OK, "truncate");
        snprintf(p, sizeof p, "%s/gamedata/dev_hdd0/game/BCUS98114/USRDIR/PDIPFS/A/1", root);
        d = slurp(p, &m);
        CHECK(m == 50 && d && !memcmp(d, e1, 50), "A1 truncated to 50 bytes of the real contents (%ld)", m);
        free(d);
    } else if (!strcmp(mode, "off")) {          /* PS3_FS_VOLMAP=0 */
        CHECK(stat_size(A1, &rc) == 256, "records are plain files when off");
    } else if (!strcmp(mode, "headbad")) {      /* volume's first bytes changed */
        rc = cellFsOpen(gp(A1), CELL_FS_O_RDONLY, (CellFsFd*)(uintptr_t)G_FD, NULL, 0);
        CHECK(rc == (s32)CELL_EIO, "open with a changed volume rc %X", rc);
        rc = cellFsOpen(gp(A1), CELL_FS_O_RDWR, (CellFsFd*)(uintptr_t)G_FD, NULL, 0);
        CHECK(rc == (s32)CELL_EIO, "copy on write with a changed volume rc %X", rc);
    } else if (!strcmp(mode, "midbad")) {       /* a middle byte changed */
        rc = cellFsOpen(gp(A1), CELL_FS_O_RDONLY, (CellFsFd*)(uintptr_t)G_FD, NULL, 0);
        int full = getenv("PS3_FS_VOLMAP") && !strcmp(getenv("PS3_FS_VOLMAP"), "2");
        CHECK(full ? rc == (s32)CELL_EIO : rc == CELL_OK, "middle change: mode %d rc %X", full ? 2 : 1, rc);
    } else if (!strcmp(mode, "nodisc")) {       /* volume missing */
        rc = cellFsOpen(gp(A1), CELL_FS_O_RDONLY, (CellFsFd*)(uintptr_t)G_FD, NULL, 0);
        CHECK(rc == (s32)CELL_EIO, "open with the volume missing rc %X", rc);
        CHECK(stat_size(A1, &rc) == (u64)n1, "stat still reports the range");
    } else {
        return 2;
    }
    if (!fails) printf("volmap harness %s: ok\n", mode);
    return fails ? 1 : 0;
}
