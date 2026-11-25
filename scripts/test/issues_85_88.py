"""Regression tests for 0.3.1 fixes to GitHub issues #85, #86, #87, #88."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
RUNTIME = Path(os.environ.get("PS3RECOMP_DIR", ROOT.parent / "ps3recomp-tourline5"))
sys.path.insert(0, str(ROOT / "scripts"))


def test_86_cull_front_face():
    backend = (RUNTIME / "libs/video/rsx_d3d12_backend.c").read_text(encoding="utf-8")
    assert "RSX_CULL_FRONT_FACE_ALWAYS" in backend, "missing RSX_CULL_FRONT_FACE_ALWAYS"
    start = backend.index("static int rsx_depth_clip_on(void)")
    end = backend.index("static u32 rsx_blend_key(", start)
    snippet = backend[start:end]

    work = Path(tempfile.mkdtemp(prefix="test-86-", dir=ROOT / "scratch"))
    (work / "cull_key.inc").write_text(snippet, encoding="utf-8")
    (work / "test.c").write_text(
        r'''
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
typedef uint32_t u32;
typedef struct {
    u32 zmin_max_control;
    int depth_test_enable;
    int cull_face_enable;
    u32 cull_face;
    u32 front_face;
    u32 shader_program;
} rsx_state;

#include "cull_key.inc"

int main(void) {
    int want_ff = !(getenv("RSX_CULL_FRONT_FACE_ALWAYS") && getenv("RSX_CULL_FRONT_FACE_ALWAYS")[0] == '0');
    rsx_state st = {0};
    st.zmin_max_control = 1u;
    st.depth_test_enable = 1;

    /* 1. Two-sided draw (cull off), CCW front face (mirror) */
    st.cull_face_enable = 0;
    st.front_face = 0x0901u;
    u32 k = rsx_cull_key(&st);
    u32 exp = want_ff ? 4u : 0u;
    if (k != exp) { fprintf(stderr, "FAIL 1: got 0x%X expected 0x%X\n", k, exp); return 1; }

    /* 2. Two-sided draw (cull off), CW front face */
    st.front_face = 0x0900u;
    k = rsx_cull_key(&st);
    if (k != 0u) { fprintf(stderr, "FAIL 2: got 0x%X expected 0\n", k); return 1; }

    /* 3. Culled draw (cull on), BACK + CCW */
    st.cull_face_enable = 1;
    st.cull_face = 0x0405u;
    st.front_face = 0x0901u;
    k = rsx_cull_key(&st);
    if (k != 5u) { fprintf(stderr, "FAIL 3: got 0x%X expected 5\n", k); return 1; }

    /* 4. CULL_OFF_FP match still preserves bit 2 when enabled */
    st.shader_program = 0x12345678u;
    k = rsx_cull_key(&st);
    exp = want_ff ? 4u : 0u;
    if (k != exp) { fprintf(stderr, "FAIL 4: got 0x%X expected 0x%X\n", k, exp); return 1; }

    /* 5. Depth clip disabled + two-sided CCW */
    st.cull_face_enable = 0;
    st.depth_test_enable = 0;
    k = rsx_cull_key(&st);
    exp = 8u | (want_ff ? 4u : 0u);
    if (k != exp) { fprintf(stderr, "FAIL 5: got 0x%X expected 0x%X\n", k, exp); return 1; }

    return 0;
}
''',
        encoding="utf-8",
    )
    (work / "build.cmd").write_text(
        f'@echo off\n'
        f'call "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\n'
        f'if errorlevel 1 exit /b 1\n'
        f'"C:\\Program Files\\LLVM\\bin\\clang-cl.exe" /nologo /TC /O2 /MD /D_CRT_SECURE_NO_WARNINGS '
        f'/I"{work}" /Fo"{work}/test.obj" /Fe"{work}/test.exe" "{work}/test.c"\n',
        encoding="ascii",
    )
    subprocess.run(["cmd", "/c", str(work / "build.cmd")], check=True)
    env_on = dict(os.environ, CULL_OFF_FP="12345678")
    env_on.pop("RSX_CULL_FRONT_FACE_ALWAYS", None)
    subprocess.run([str(work / "test.exe")], env=env_on, check=True)
    env_off = dict(env_on, RSX_CULL_FRONT_FACE_ALWAYS="0")
    subprocess.run([str(work / "test.exe")], env=env_off, check=True)
    print("[PASS] #86 rsx_cull_key preserves front_face CCW when culling is off (default & =0 rollback)")


def test_88_offscreen_frame_budget():
    backend = (RUNTIME / "libs/video/rsx_d3d12_backend.c").read_text(encoding="utf-8")
    assert "RSX_PSO_WAIT_OFFSCREEN_FRAME_MS" in backend, "missing RSX_PSO_WAIT_OFFSCREEN_FRAME_MS"
    assert "rem_frame < (ULONGLONG)wait_ms" in backend, "missing per-draw vs per-frame clamp"
    assert "took >= (ULONGLONG)wait_ms && ngu < 64" in backend, "gave_up[] must only record per-draw timeouts"
    print("[PASS] #88 offscreen PSO wait separates per-draw (500 ms) and per-frame (4000 ms) budgets")


def test_85_bad_branch_retry():
    gcm = (RUNTIME / "libs/video/cellGcmSys.c").read_text(encoding="utf-8")
    assert "GCM_BAD_BRANCH_RETRY_MS" in gcm, "missing GCM_BAD_BRANCH_RETRY_MS"
    assert 'why = "unmapped-jump-retry"' in gcm, "missing unmapped-jump-retry park"
    assert 'why = "unmapped-call-retry"' in gcm, "missing unmapped-call-retry park"
    print("[PASS] #85 cellGcmSys retries unmapped JUMP/CALL briefly before resyncing to put")


def test_87_guest_eh_pad_body():
    import guest_eh_patch

    targets = json.loads((ROOT / "config/targets.json").read_text(encoding="utf-8"))
    sites = targets["BCUS98114-v211"]["guest_eh"]["catch_sites"]
    site = next((s for s in sites if s["func"] == "0x009F67A4"), None)
    assert site is not None, "missing 0x009F67A4 catch site in BCUS98114-v211"
    assert site["call"] == "0x009F683C"
    assert site["pad"] == "0x009F6850"
    assert len(site["pad_body"]) == 13

    sample = (
        "void func_009F67A4(ppu_context* ctx) {\n"
        "        ctx->gpr[3] = ctx->gpr[31] | ctx->gpr[31];\n"
        "        ctx->gpr[4] = ctx->gpr[29] | ctx->gpr[29];\n"
        "        ctx->lr = 0x009F6840; func_00A5CF54(ctx); DRAIN_TRAMPOLINE(ctx);\n"
        "        ctx->gpr[3] = ctx->gpr[30] | ctx->gpr[30];\n"
        "        ctx->gpr[26] = (int64_t)(int32_t)(1);\n"
        "        ctx->lr = 0x009F684C; func_009BE4EC(ctx); DRAIN_TRAMPOLINE(ctx);\n"
        "        { g_trampoline_fn = (void(*)(void*))func_009F6870; return; }\n"
        "}\n"
    )
    patched = guest_eh_patch.patch_site(sample, site)
    assert "gt5p_eh_arm(ctx, 0x009F67A4u)" in patched
    assert "goto loc_009F6850;" in patched
    assert "loc_009F6850: /* guest_eh_patch */" in patched
    assert "func_00BE8A64(ctx);" in patched
    assert "func_00BE8964(ctx);" in patched
    assert "/* guest_eh_patch end */" in patched
    restored = guest_eh_patch.unpatch_site(patched, site)
    assert restored == sample, f"unpatch_site did not round-trip:\n{restored}"

    real_gen = Path(r"C:\pt5-release\0.1.15\v211\src\generated\ppu_recomp_004.cpp")
    if real_gen.exists():
        txt = real_gen.read_text(encoding="utf-8", errors="surrogateescape")
        i = txt.index("void func_009F67A4(")
        j = txt.index("\n}\n", i) + 3
        real_body = txt[i:j]
        rp = guest_eh_patch.patch_site(real_body, site)
        rr = guest_eh_patch.unpatch_site(rp, site)
        assert rr == real_body, "real func_009F67A4 did not round-trip"
        print("[PASS] #87 guest_eh_patch pad_body round-trips on fixture and real 2.11 func_009F67A4")
    else:
        print("[PASS] #87 guest_eh_patch pad_body round-trips on fixture")


if __name__ == "__main__":
    test_86_cull_front_face()
    test_88_offscreen_frame_budget()
    test_85_bad_branch_retry()
    test_87_guest_eh_pad_body()
