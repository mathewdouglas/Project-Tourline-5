/* Production cellSaveData.c, a synthetic game callback, no game data.
 *
 * GT5 2.11 splits a save over 2 MiB into GT5.0, GT5.1, ... A load must put
 * the file list in the game's setBuf->buf, where its part loop looks for
 * GT5.1 (#94/#95/#99). A save asks for reCreateMode YES: a smaller save must
 * not leave an old GT5.1 behind, since the next load reads every GT5.N. */
#include "cellSaveData.c"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

uint8_t* vm_base;
uint32_t ppu_vm_size = 0x04000000;   /* the test's committed guest memory */
ps3_guest_caller_fn g_ps3_guest_caller;
void pad_milestone_mark(const char* name) { (void)name; }
/* Guest-memory coherence: no SPU here, so nothing is ever reserved. */
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; CHECK(0); }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_lockline_lock(void) { CHECK(0); }
void spu_lockline_unlock(void) { CHECK(0); }
void spu_coh_notify_write(uint32_t a) { (void)a; CHECK(0); }
void ppu_resv_break_store(uint64_t a) { (void)a; CHECK(0); }
void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*))
{ (void)nid; (void)name; (void)fn; }

#define OPD_STAT 0x100u
#define OPD_FILE 0x200u
#define NAMES    0x03000000u   /* guest file names */
#define BUF      0x03100000u   /* guest file buffer */

static u32 s_recreate;         /* what the "game" puts in reCreateMode */
static const char* s_parts[8]; /* files the "game" writes, in order */
static u32 s_sizes[8];
static int s_next;
static int s_bad_at = -1;      /* write an invalid name at this step: the op fails */

/* Load, as GT5 2.11 does it: the stat callback reads statGet->fileList
 * (+1636, fileListNum +1632); the part loop then finds GT5.1, GT5.2, ... in
 * its own setBuf->buf, never through statGet. */
#define SETBUF   0x03800000u   /* past BUF's 2 MiB */
#define LISTBUF  0x03810000u
#define OPD_LSTAT 0x300u
#define OPD_LFILE 0x400u
static u32 s_list_ptr, s_list_num;
static int s_load_step;
static char s_loaded[8][16];

static int listed(u32 list, const char* name)
{
    for (u32 i = 0; i < s_list_num; i++)
        if (!strcmp((const char*)vm_base + list + i * 56 + 40, name)) return 1;
    return 0;
}

static void game(uint32_t opd, uint64_t cb, uint64_t get, uint64_t set,
                 uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7)
{
    (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    if (opd == OPD_LSTAT) {
        s_list_num = vm_read32((u32)get + 1632);
        s_list_ptr = vm_read32((u32)get + 1636);
        vm_write32((u32)cb, (u32)CELL_SAVEDATA_CBRESULT_OK_NEXT);
        return;
    }
    if (opd == OPD_LFILE) {
        char name[16]; snprintf(name, sizeof(name), "GT5.%d", s_load_step);
        if (!listed(s_load_step ? LISTBUF : s_list_ptr, name)) { vm_write32((u32)cb, (u32)CELL_SAVEDATA_CBRESULT_OK_LAST); return; }
        strcpy(s_loaded[s_load_step], name);
        const u32 nm = NAMES + (u32)s_load_step * 16;
        strcpy((char*)vm_base + nm, name);
        vm_write32((u32)set + 0, CELL_SAVEDATA_FILEOP_READ);
        vm_write32((u32)set + 8, CELL_SAVEDATA_FILETYPE_SECUREFILE);
        vm_write32((u32)set + 28, nm);
        vm_write32((u32)set + 36, 0x00200000);
        vm_write32((u32)set + 40, 0x00200000);
        vm_write32((u32)set + 44, BUF);
        vm_write32((u32)cb, (u32)CELL_SAVEDATA_CBRESULT_OK_NEXT);
        s_load_step++;
        return;
    }
    if (opd == OPD_STAT) {
        vm_write32((u32)set + 4, s_recreate);
        vm_write32((u32)cb, (u32)CELL_SAVEDATA_CBRESULT_OK_NEXT);
        return;
    }
    if (!s_parts[s_next]) { vm_write32((u32)cb, (u32)CELL_SAVEDATA_CBRESULT_OK_LAST); return; }
    const u32 name = NAMES + (u32)s_next * 16;
    strcpy((char*)vm_base + name, s_next == s_bad_at ? "a/b" : s_parts[s_next]);
    memset(vm_base + BUF, 'A' + s_next, s_sizes[s_next]);
    vm_write32((u32)set + 0, CELL_SAVEDATA_FILEOP_WRITE);
    vm_write32((u32)set + 8, CELL_SAVEDATA_FILETYPE_SECUREFILE);
    vm_write32((u32)set + 28, name);
    vm_write32((u32)set + 32, 0);
    vm_write32((u32)set + 36, s_sizes[s_next]);
    vm_write32((u32)set + 40, s_sizes[s_next]);
    vm_write32((u32)set + 44, BUF);
    vm_write32((u32)cb, (u32)CELL_SAVEDATA_CBRESULT_OK_NEXT);
    s_next++;
}

static char s_dir[1024];

static long file_size(const char* name)
{
    char p[1100]; snprintf(p, sizeof(p), "%s/%s", s_dir, name);
    HOST_STAT_T st;
    return HOST_STAT(p, &st) == 0 ? (long)st.st_size : -1;
}

static void put(const char* name, long size)
{
    char p[1100]; snprintf(p, sizeof(p), "%s/%s", s_dir, name);
    FILE* f = fopen(p, "wb"); CHECK(f);
    for (long i = 0; i < size; i++) fputc('o', f);
    fclose(f);
}

/* An old save that had grown past 2 MiB: GT5.0 and GT5.1. */
static void old_split_save(void)
{
    ensure_dirs(s_dir);
    put("PARAM.SFO", 10);
    put("GT5.0", 2097152);
    put("GT5.1", 1000);
    put("ICON0.PNG", 50);
    pfd_add(s_dir, "GT5.0");
    pfd_add(s_dir, "GT5.1");
}

static s32 save(u32 recreate, const char* a, u32 asz, const char* b, u32 bsz)
{
    s_recreate = recreate; s_next = 0;
    memset(s_parts, 0, sizeof(s_parts));
    s_parts[0] = a; s_sizes[0] = asz; s_parts[1] = b; s_sizes[1] = bsz;
    return savedata_execute("BCUS98114-GAME", 1, NULL, (CellSaveDataStatCallback)(uintptr_t)OPD_STAT,
                            (CellSaveDataFileCallback)(uintptr_t)OPD_FILE, NULL);
}

int main(int argc, char** argv)
{
    CHECK(argc == 2);
    vm_base = (uint8_t*)VirtualAlloc(NULL, 0x04000000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(vm_base);
    g_ps3_guest_caller = game;
    snprintf(s_save_root, sizeof(s_save_root), "%s", argv[1]);
    build_save_path(s_dir, sizeof(s_dir), "BCUS98114-GAME");

    /* 1. RECREATE_YES, the save shrank under 2 MiB: GT5.1 goes. */
    old_split_save();
    CHECK(save(CELL_SAVEDATA_RECREATE_YES, "GT5.0", 300000, NULL, 0) == CELL_OK);
    CHECK(file_size("GT5.0") == 300000);
    CHECK(file_size("GT5.1") == -1);
    CHECK(file_size("ICON0.PNG") == -1);     /* not written by this save */
    CHECK(file_size("PARAM.SFO") == 10);     /* system files stay */
    CHECK(pfd_has(s_dir, "GT5.0") && !pfd_has(s_dir, "GT5.1"));
    printf("recreate: stale GT5.1 removed\n");

    /* 2. RECREATE_YES, still split: both parts stay. */
    old_split_save();
    CHECK(save(CELL_SAVEDATA_RECREATE_YES, "GT5.0", 2097152, "GT5.1", 4000) == CELL_OK);
    CHECK(file_size("GT5.0") == 2097152 && file_size("GT5.1") == 4000);
    printf("recreate: written parts kept\n");

    /* 3. RECREATE_NO keeps everything (the console does too). */
    old_split_save();
    CHECK(save(0, "GT5.0", 300000, NULL, 0) == CELL_OK);
    CHECK(file_size("GT5.1") == 1000 && file_size("ICON0.PNG") == 50);
    printf("recreate NO: files kept\n");

    /* 4. A save that fails part way removes nothing. */
    old_split_save();
    s_bad_at = 1;
    CHECK(save(CELL_SAVEDATA_RECREATE_YES, "GT5.0", 300000, "GT5.1", 10) != CELL_OK);
    s_bad_at = -1;
    CHECK(file_size("GT5.1") == 1000 && file_size("ICON0.PNG") == 50);
    printf("recreate: failed save removes nothing\n");

    /* 5. PS3_SAVEDATA_RECREATE=0: the old behaviour. */
    old_split_save();
    _putenv("PS3_SAVEDATA_RECREATE=0");
    CHECK(save(CELL_SAVEDATA_RECREATE_YES, "GT5.0", 300000, NULL, 0) == CELL_OK);
    CHECK(file_size("GT5.1") == 1000);
    _putenv("PS3_SAVEDATA_RECREATE=");
    printf("PS3_SAVEDATA_RECREATE=0: stale GT5.1 kept\n");

    /* 6. Load of a split save: the list is in setBuf->buf, so GT5.1 is read. */
    old_split_save();
    vm_write32(SETBUF + 0, 2048);           /* dirListMax */
    vm_write32(SETBUF + 4, 113);            /* fileListMax, GT5's */
    vm_write32(SETBUF + 32, 0x18000);       /* bufSize, GT5's */
    vm_write32(SETBUF + 36, LISTBUF);       /* buf */
    s_load_step = 0; memset(s_loaded, 0, sizeof(s_loaded));
    CHECK(savedata_execute("BCUS98114-GAME", 0, (CellSaveDataSetBuf*)(uintptr_t)SETBUF,
                           (CellSaveDataStatCallback)(uintptr_t)OPD_LSTAT,
                           (CellSaveDataFileCallback)(uintptr_t)OPD_LFILE, NULL) == CELL_OK);
    CHECK(s_list_ptr == LISTBUF && s_list_num == 3);
    CHECK(!strcmp(s_loaded[0], "GT5.0") && !strcmp(s_loaded[1], "GT5.1") && !s_loaded[2][0]);
    printf("load: file list in setBuf->buf, GT5.0 and GT5.1 read\n");

    /* 7. PS3_SAVEDATA_LIST_IN_BUF=0: the old scratch list; GT5.1 is missed. */
    memset(vm_base + LISTBUF, 0, 0x18000);
    _putenv("PS3_SAVEDATA_LIST_IN_BUF=0");
    s_load_step = 0; memset(s_loaded, 0, sizeof(s_loaded));
    CHECK(savedata_execute("BCUS98114-GAME", 0, (CellSaveDataSetBuf*)(uintptr_t)SETBUF,
                           (CellSaveDataStatCallback)(uintptr_t)OPD_LSTAT,
                           (CellSaveDataFileCallback)(uintptr_t)OPD_LFILE, NULL) == CELL_OK);
    _putenv("PS3_SAVEDATA_LIST_IN_BUF=");
    CHECK(s_list_ptr != LISTBUF && !strcmp(s_loaded[0], "GT5.0") && !s_loaded[1][0]);
    printf("PS3_SAVEDATA_LIST_IN_BUF=0: only GT5.0 read (the #99 failure)\n");

    printf("PASS\n");
    return 0;
}
