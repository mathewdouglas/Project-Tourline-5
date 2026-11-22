"""The car render job's native triangle loop (src/gt5p_native_cull.c, jump hook at b0196FF40
0xAF88) against the lifted loop, on captured real states of the cull (entry 0xAB00): each state
runs the whole cull to its return in both builds; local store, the return pc and every register
but the SPU ABI's volatile r2-r79 must match. Run once for the copy that computes fma through
doubles (default build) and once for an FMA copy (-mfma).

Capture first (game-derived data: keep it in scratch/):
    .venv\\Scripts\\python scripts\\diag\\spu_capture_patch.py b0196FF40 AB00
    (build) then a Rome race (gt5boot.ps1 -Route race -Tab 2) with
    GT5P_SPU_CAPTURE=scratch\\pv\\cullcap.bin GT5P_SPU_CAPTURE_N=60 GT5P_SPU_CAPTURE_GAP_MS=1500 GT5P_SPU_CAPTURE_AFTER_MS=0
    .venv\\Scripts\\python scripts\\diag\\spu_capture_patch.py --restore b0196FF40
Then (generated/ with spu_native_patch.py applied):
    .venv\\Scripts\\python scripts\\test\\native_cull.py [--states scratch\\pv\\cullcap.bin]
"""
from pathlib import Path
import argparse, os, subprocess, sys
root = Path(__file__).resolve().parents[2]
ap = argparse.ArgumentParser()
ap.add_argument("--states", default=str(root / "scratch" / "pv" / "cullcap.bin"))
ap.add_argument("--reps", type=int, default=5)
a = ap.parse_args()
if "gt5p_cull_af88" not in (root / "generated/spu_lifted/b0196FF40/spu_recomp.c").read_text(encoding="utf-8"):
    sys.exit("the hook is not in generated/: run scripts/spu_native_patch.py")
env = dict(os.environ, BENCH_IGNORE_REGS="2-79")
rc = 0
for fma in ("", "-mfma,"):
    print("== fma/fnms", "fused (-mfma)" if fma else "through doubles")
    r = subprocess.run([sys.executable, str(root / "scripts/test/spu_loop_bench.py"), "--image", "b0196FF40",
                        "--states", a.states, "--reps", str(a.reps),
                        "--variant", f"lifted={fma}/DNC_FORCE_OFF", "--variant", f"native={fma}/DSPU_DUMMY"], env=env)
    rc |= r.returncode
sys.exit(rc)
