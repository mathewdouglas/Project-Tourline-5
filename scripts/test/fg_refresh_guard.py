"""Frame generation refresh guard (RSX_FG_REFRESH_GUARD).

Every generated frame is a present, and flip-model presents cannot outrun the
display. On a 100 Hz display 2x frame generation held GT5 at 50 real FPS (a
High Speed Ring race: 45.6 FPS, 73% of frames late; without generation 59.4).
The backend now generates only as many frames as the window's display can
show at the game's 60 FPS. This test compiles the real fg_fit_for_hz() from
the backend source and checks its table, and checks that the guard runs
before the feature is made and that its rollback switch is present.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
RUNTIME = Path(os.environ.get("PS3RECOMP_DIR", ROOT.parent / "ps3recomp-tourline5"))

# display Hz -> generated frames per real frame (0 = paused)
EXPECT = {
    0: 0, 59: 0, 60: 0, 75: 0, 99: 0, 100: 0, 110: 0, 116: 0,
    117: 1, 119: 1, 120: 1, 144: 1, 165: 1, 174: 1,
    175: 2, 200: 2, 233: 2,
    234: 3, 240: 3, 360: 3, 500: 3,
}


def compiler():
    for c in ("clang", "clang-cl", "gcc", "cl"):
        if shutil.which(c):
            return c
    llvm = Path(r"C:\Program Files\LLVM\bin\clang.exe")
    return str(llvm) if llvm.exists() else None


def test_fit_table():
    src = (RUNTIME / "libs/video/rsx_d3d12_backend.c").read_text(encoding="utf-8")
    start = src.index("static u32 fg_fit_for_hz(u32 hz)")
    end = src.index("static u32 fg_refresh_fit(void)", start)
    work = Path(tempfile.mkdtemp(prefix="fg-guard-", dir=ROOT / "scratch"))
    (work / "t.c").write_text(
        "#include <stdio.h>\n#include <stdint.h>\ntypedef uint32_t u32;\n"
        + src[start:end]
        + "int main(void){ u32 hz; while (scanf(\"%u\", &hz) == 1) printf(\"%u %u\\n\", hz, fg_fit_for_hz(hz)); return 0; }\n",
        encoding="utf-8")
    cc = compiler()
    assert cc, "no C compiler found"
    exe = work / "t.exe"
    if cc.endswith("cl") and not cc.endswith("clang"):
        subprocess.run([cc, "/nologo", str(work / "t.c"), "/Fe" + str(exe)], cwd=work, check=True,
                       capture_output=True)
    else:
        subprocess.run([cc, "-D_CRT_SECURE_NO_WARNINGS", str(work / "t.c"), "-o", str(exe)], check=True)
    out = subprocess.run([str(exe)], input=" ".join(map(str, EXPECT)), capture_output=True,
                         text=True, check=True).stdout.split("\n")
    got = {int(a): int(b) for a, b in (l.split() for l in out if l.strip())}
    bad = {hz: (got.get(hz), want) for hz, want in EXPECT.items() if got.get(hz) != want}
    assert not bad, f"fg_fit_for_hz mismatches (got, want): {bad}"
    shutil.rmtree(work, ignore_errors=True)
    print(f"[PASS] fg_fit_for_hz: {len(EXPECT)} refresh rates (2x from 117 Hz, 3x 175, 4x 234)")


def test_guard_wiring():
    src = (RUNTIME / "libs/video/rsx_d3d12_backend.c").read_text(encoding="utf-8")
    assert 'getenv("RSX_FG_REFRESH_GUARD")' in src, "missing RSX_FG_REFRESH_GUARD rollback"
    ev = src.index("static int fg_eval_frame(u32 fi)")
    guard = src.index("const u32 fit = fg_refresh_fit();", ev)
    init = src.index("if (!fg_ensure_init()) return 0;", ev)
    assert guard < init, "refresh guard must run before the FG feature is created"
    assert "if (gen > fit) gen = fit;" in src[ev:ev + 6000]
    assert "s_fg_paused" in src[src.index("static void fg_hudless_capture(u32 fi)"):][:400], \
        "hudless capture must stop while generation is paused"
    print("[PASS] guard runs before FG init, clamps gen, pauses the hudless copy; rollback switch present")


if __name__ == "__main__":
    test_fit_table()
    test_guard_wiring()
