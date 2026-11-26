"""Self-read snapshots: copy only what is read, share one per texture-cache invalidate.

GT5's headlamp glass samples the scene it draws into (unit 10). Every glass
draw took a full snapshot of the 3440x1440 scene: three layers per premium car,
in the main view and the rear-view mirror, up to 1.3 GB of copies a frame in a
user's Monza report (RTX 4060, GPU-bound at 23 FPS). The game draws the three
layers after one texture-cache invalidate, so the RSX could not have shown one
layer what the layer before it wrote. Draws into a target with the same
invalidate count, viewport and scissor now share a snapshot (RSX_SELF_EPOCH),
only the targets the program samples are copied (RSX_SELF_READ_ONLY), and a
multisampled target is resolved straight into the snapshot (RSX_SELF_RESOLVE).

This compiles the real self_region() and self_capture_at() from the backend
with stub copies and checks when a snapshot is shared and when it is taken
again, then checks the wiring in the source.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
RUNTIME = Path(os.environ.get("PS3RECOMP_DIR", ROOT.parent / "ps3recomp-tourline5"))

HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
typedef unsigned char u8; typedef unsigned u32; typedef long LONG; typedef unsigned UINT;
typedef int ID3D12Resource;
typedef struct { LONG left, top, right, bottom; } D3D12_RECT;
typedef struct { UINT left, top, front, right, bottom, back; } D3D12_BOX;
typedef struct { ID3D12Resource *res, *self, *ms; u32 w, h; } OffRT;
#define MAX_OFF_RTS 8
static struct { OffRT off_rt[MAX_OFF_RTS]; } s_d3d;
static LONG InterlockedIncrement(volatile LONG* p) { return ++*p; }
static int copies, regions; static D3D12_BOX last_box;
static void off_rt_self_capture_box(int slot, const D3D12_BOX* box)
{ (void)slot; copies++; if (box) { regions++; last_box = *box; } }
SOURCE
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)
int main(void)
{
    static ID3D12Resource res, self, self2;
    s_d3d.off_rt[1].res = &res; s_d3d.off_rt[1].self = &self;
    s_d3d.off_rt[1].w = 3440; s_d3d.off_rt[1].h = 1440;
    const float full[4] = {0, 0, 3440, 1440};
    const D3D12_RECT sc = {0, 0, 3440, 1440};
    const int share = getenv("RSX_SELF_EPOCH") == NULL;
    /* three glass layers after one invalidate */
    self_capture_at(1, 7, full, &sc); self_capture_at(1, 7, full, &sc); self_capture_at(1, 7, full, &sc);
    CHECK(copies == (share ? 1 : 3));
    CHECK(s_self_shared == (share ? 2 : 0));
    /* the next car invalidates first */
    copies = 0; self_capture_at(1, 8, full, &sc); CHECK(copies == 1);
    /* a mip level drawn after the one above it: another viewport */
    const float lvl[4] = {0, 0, 1720, 720};
    copies = 0; self_capture_at(1, 8, lvl, &sc); CHECK(copies == 1);
    /* another scissor */
    const D3D12_RECT msc = {0, 0, 1376, 341};
    copies = 0; self_capture_at(1, 8, lvl, &msc); CHECK(copies == 1);
    /* the snapshot was re-created */
    s_d3d.off_rt[1].self = &self2;
    copies = 0; self_capture_at(1, 8, lvl, &msc); CHECK(copies == 1);
    /* a new replay starts empty */
    memset(s_self_last, 0, sizeof s_self_last);
    copies = 0; self_capture_at(1, 8, lvl, &msc); CHECK(copies == 1);
    /* whole-target copies unless the experiment is on */
    CHECK(regions == (getenv("RSX_SELF_SCISSOR") ? 4 : 0));
    if (getenv("RSX_SELF_SCISSOR")) {
        CHECK(last_box.right == 1376 && last_box.bottom == 341);
        const float off[4] = {4000, 0, 100, 100};
        copies = 0; self_capture_at(1, 9, off, &sc); CHECK(copies == 0);   /* draws nothing */
    }
    printf("ok copies shared=%ld\n", (long)s_self_shared);
    return 0;
}
'''


def compiler():
    for c in ("clang", "gcc"):
        if shutil.which(c):
            return c
    llvm = Path(r"C:\Program Files\LLVM\bin\clang.exe")
    return str(llvm) if llvm.exists() else None


def test_rule():
    src = (RUNTIME / "libs/video/rsx_d3d12_backend.c").read_text(encoding="utf-8")
    start = src.index("typedef struct { ID3D12Resource* res; u32 ep; float vp[4]; D3D12_RECT sc; u8 valid; } SelfLast;")
    end = src.index("static void off_rt_self_capture_box(int slot, const D3D12_BOX* box)\n{", start)
    work = Path(tempfile.mkdtemp(prefix="self-share-", dir=ROOT / "scratch"))
    (work / "t.c").write_text(HARNESS.replace("SOURCE", src[start:end]), encoding="utf-8")
    cc = compiler()
    assert cc, "no C compiler found"
    exe = work / "t.exe"
    subprocess.run([cc, "-D_CRT_SECURE_NO_WARNINGS", str(work / "t.c"), "-o", str(exe)], check=True)
    for env in ({}, {"RSX_SELF_EPOCH": "0"}, {"RSX_SELF_SCISSOR": "1"}):
        r = subprocess.run([str(exe)], env={**os.environ, **env}, capture_output=True, text=True)
        assert r.returncode == 0, f"{env}: {r.stderr.strip()}"
    shutil.rmtree(work, ignore_errors=True)
    print("[PASS] one snapshot per invalidate, viewport and scissor; new copy on any change; rollback and experiment")


def test_wiring():
    src = (RUNTIME / "libs/video/rsx_d3d12_backend.c").read_text(encoding="utf-8")
    for e in ("RSX_SELF_EPOCH", "RSX_SELF_READ_ONLY", "RSX_SELF_RESOLVE", "RSX_SELF_SCISSOR", "RSX_SELF_TRACE"):
        assert f'getenv("{e}")' in src, e
    assert "on = (e && e[0] == '1'); }" in src[src.index('getenv("RSX_SELF_SCISSOR")') - 120:][:200], \
        "the region copy must stay opt-in"
    assert "dr->inv_epoch = g_rsx_tex_inval;" in src
    cmds = (RUNTIME / "libs/video/rsx_commands.c").read_text(encoding="utf-8")
    inv = cmds[cmds.index("if (method == NV4097_INVALIDATE_L2) {"):][:600]
    assert "if (data & 1u) g_rsx_tex_inval++;" in inv
    rep = src[src.index("if (dr->tex_self && cur_rt >= 0) {"):][:2600]
    assert "want |= 1u << ((dr->self_tgt >> (2 * _u)) & 3u);" in rep
    assert "memset(s_self_last, 0, sizeof s_self_last);" in src
    cap = src[src.index("static void off_rt_self_capture_box(int slot, const D3D12_BOX* box)\n{"):][:2400]
    assert "ResolveSubresource(s_d3d.cmd_list, r->self, 0, r->ms, 0," in cap
    assert "r->ms_dirty = 0" not in cap, "a resolve into the snapshot must leave res out of date"
    print("[PASS] invalidate count recorded per draw; only read targets copied; direct resolve keeps res dirty")


if __name__ == "__main__":
    test_rule()
    test_wiring()
