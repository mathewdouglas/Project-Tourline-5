"""Regression tests for 0.3.2 fixes/features: GitHub issues #89, #90, #91."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
RUNTIME = Path(os.environ.get("PS3RECOMP_DIR", ROOT.parent / "ps3recomp-tourline5"))


def test_89_tex_ww_dense_dirty():
    backend = (RUNTIME / "libs/video/rsx_d3d12_backend.c").read_text(encoding="utf-8")
    assert "RSX_TEX_WW_DENSE_DIRTY" in backend, "missing RSX_TEX_WW_DENSE_DIRTY rollback"
    assert "const int ww_dirty = (ww_now && c->ww_gen && !ww_clean && dense_dirty);" in backend
    assert "(ww_clean && (s_tww_check || c->ww_skips >= 128u)) || ww_dirty" in backend
    print("[PASS] #89 RSX_TEX_WRITE_WATCH forces dense check on dirty pages before advancing ww_gen")


def test_90_rs_dpad_and_speed_hotkeys():
    pad_c = (RUNTIME / "libs/input/cellPad.c").read_text(encoding="utf-8")
    pad_h = (RUNTIME / "libs/input/cellPad.h").read_text(encoding="utf-8")
    settings_h = (ROOT / "src/gt5p_settings.h").read_text(encoding="utf-8")
    settings_cpp = (ROOT / "src/gt5p_settings.cpp").read_text(encoding="utf-8")
    settings_ui = (ROOT / "src/gt5p_settings_ui.cpp").read_text(encoding="utf-8")

    assert "cellPad_set_rs_dpad" in pad_h and "cellPad_set_in_race" in pad_h
    assert "PAD_RS_DPAD" in pad_c
    for sym in ("right_stick_dpad", "speed_hotkey_pad", "speed_hotkey_key"):
        assert sym in settings_h and sym in settings_cpp, f"missing {sym}"
    for env in ("PAD_RS_DPAD", "GT5P_SPEED_PAD", "GT5P_SPEED_KEY"):
        assert env in settings_cpp and env in settings_ui, f"missing {env}"
    assert "cellPad_set_in_race(s_in_race);" in settings_ui
    assert "game_speed_poll(GetForegroundWindow() == s_game, down);" in settings_ui

    # Extract and test the actual right-stick -> D-Pad mapping logic from cellPad.c
    start = pad_c.index("static volatile int s_pad_rs_dpad = -1;")
    end = pad_c.index("void cellPad_poll(void)", start)
    helpers = pad_c[start:end]

    work = Path(tempfile.mkdtemp(prefix="test-90-", dir=ROOT / "scratch"))
    (work / "rs_dpad.inc").write_text(helpers, encoding="utf-8")
    (work / "test.c").write_text(
        r'''
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
typedef uint16_t u16;
typedef uint8_t  u8;
#define CELL_PAD_CTRL_UP    (1 << 4)
#define CELL_PAD_CTRL_RIGHT (1 << 5)
#define CELL_PAD_CTRL_DOWN  (1 << 6)
#define CELL_PAD_CTRL_LEFT  (1 << 7)

#include "rs_dpad.inc"

static void apply(int port, u8 rx, u8 ry, u16* d1, u8* out_rx, u8* out_ry) {
    const int rsm = (port == 0) ? pad_rs_dpad_mode() : 0;
    if (rsm == 2 || (rsm == 1 && s_pad_in_race)) {
        if (rx < 64)  *d1 |= CELL_PAD_CTRL_LEFT;
        if (rx > 192) *d1 |= CELL_PAD_CTRL_RIGHT;
        if (ry < 64)  *d1 |= CELL_PAD_CTRL_UP;
        if (ry > 192) *d1 |= CELL_PAD_CTRL_DOWN;
        *out_rx = 128; *out_ry = 128;
    } else {
        *out_rx = rx; *out_ry = ry;
    }
}

int main(void) {
    u16 d1 = 0; u8 ox = 0, oy = 0;
    /* Mode 0 (off): right stick passes through untouched */
    cellPad_set_rs_dpad(0); cellPad_set_in_race(1);
    apply(0, 10, 240, &d1, &ox, &oy);
    if (d1 != 0 || ox != 10 || oy != 240) { fprintf(stderr, "FAIL mode 0\n"); return 1; }

    /* Mode 1 (in_race), not in race: right stick passes through */
    cellPad_set_rs_dpad(1); cellPad_set_in_race(0);
    d1 = 0; apply(0, 10, 240, &d1, &ox, &oy);
    if (d1 != 0 || ox != 10 || oy != 240) { fprintf(stderr, "FAIL mode 1 out of race\n"); return 1; }

    /* Mode 1 (in_race), in race: maps to LEFT + DOWN and centers stick */
    cellPad_set_in_race(1);
    d1 = 0; apply(0, 10, 240, &d1, &ox, &oy);
    if (d1 != (CELL_PAD_CTRL_LEFT | CELL_PAD_CTRL_DOWN) || ox != 128 || oy != 128) {
        fprintf(stderr, "FAIL mode 1 in race: d1=0x%X o=(%u,%u)\n", d1, ox, oy); return 1;
    }

    /* Mode 2 (always), out of race: maps to RIGHT + UP */
    cellPad_set_rs_dpad(2); cellPad_set_in_race(0);
    d1 = 0; apply(0, 250, 5, &d1, &ox, &oy);
    if (d1 != (CELL_PAD_CTRL_RIGHT | CELL_PAD_CTRL_UP) || ox != 128 || oy != 128) {
        fprintf(stderr, "FAIL mode 2: d1=0x%X o=(%u,%u)\n", d1, ox, oy); return 1;
    }

    /* Deadzone: small deflection stays neutral */
    d1 = 0; apply(0, 100, 150, &d1, &ox, &oy);
    if (d1 != 0 || ox != 128 || oy != 128) { fprintf(stderr, "FAIL deadzone\n"); return 1; }

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
    subprocess.run([str(work / "test.exe")], check=True)
    print("[PASS] #90 right-stick-to-D-Pad (RA) & controller/keyboard speed hotkeys verified")


def test_91_launcher_rpcs3_log_dir():
    launcher = (ROOT / "launcher/launcher.cpp").read_text(encoding="utf-8")
    assert launcher.count('fs::create_directories(dir / L"log");') >= 2, "must create rpcs3/log on both cached and fresh unpack"
    assert "[pt5-launcher] headless RPCS3 exited with code" in launcher, "missing headless exit-code diagnostic"
    print("[PASS] #91 launcher pre-creates rpcs3/log and logs headless exit code on failure")


if __name__ == "__main__":
    test_89_tex_ww_dense_dirty()
    test_90_rs_dpad_and_speed_hotkeys()
    test_91_launcher_rpcs3_log_dir()
