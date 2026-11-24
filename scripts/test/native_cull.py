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
import argparse, os, struct, subprocess, sys, tempfile
root = Path(__file__).resolve().parents[2]
ap = argparse.ArgumentParser()
ap.add_argument("--states", default=str(root / "scratch" / "pv" / "cullcap.bin"))
ap.add_argument("--reps", type=int, default=5)
ap.add_argument("--expand", action="store_true", help="also replay smaller batches and both facing signs from each captured input")
ap.add_argument("--table-only", action="store_true", help="compare the new coverage-table path with the existing native cull enabled on both sides")
a = ap.parse_args()
if a.expand:
    raw = Path(a.states).read_bytes()
    record_size = 24 + 128 * 16 + 0x40000
    if len(raw) % record_size:
        sys.exit("incomplete capture record")
    folder = Path(tempfile.mkdtemp(prefix="cull-expanded-", dir=root / "scratch"))
    expanded = folder / "states.bin"
    count = 0
    with expanded.open("wb") as out:
        for off in range(0, len(raw), record_size):
            record = raw[off:off + record_size]
            if record[:8] != b"SPUCAP1\0" or struct.unpack_from("<I", record, 8)[0] != 0xAB00:
                sys.exit("--expand requires cull-entry (0xAB00) states")
            line = struct.unpack_from("<I", record, 24 + 4 * 16)[0] & 0x3FFF0
            view = struct.unpack_from("<I", record, 24 + 5 * 16)[0] & 0x3FFF0
            ls = 24 + 128 * 16
            ntri = struct.unpack_from(">I", record, ls + line)[0]
            for n in sorted({v for v in (1, 2, 3, 4, 5, 7, 8, 15, 16, ntri // 2, ntri - 1, ntri) if 0 < v <= ntri}):
                for flip in (False, True):
                    case = bytearray(record)
                    struct.pack_into(">I", case, ls + line, n)
                    case[ls + view + 1] = 0x01 if flip else 0xFF
                    out.write(case)
                    count += 1
    a.states = str(expanded)
    print(f"expanded to {count} captured-input cases", flush=True)
if "gt5p_cull_af88" not in (root / "generated/spu_lifted/b0196FF40/spu_recomp.c").read_text(encoding="utf-8"):
    sys.exit("the hook is not in generated/: run scripts/spu_native_patch.py")
env = dict(os.environ, BENCH_IGNORE_REGS="2-79")
if a.table_only:
    env["GT5P_NATIVE_CULL_TABLE"] = "1"
rc = 0
for fma in ("", "-mfma,"):
    print("== fma/fnms", "fused (-mfma)" if fma else "through doubles")
    r = subprocess.run([sys.executable, str(root / "scripts/test/spu_loop_bench.py"), "--image", "b0196FF40",
                        "--states", a.states, "--reps", str(a.reps),
                        "--variant", f"lifted={fma}/D{'NC_TABLE_FORCE_OFF' if a.table_only else 'NC_FORCE_OFF'}",
                        "--variant", f"native={fma}/D{'NC_TABLE_TEST' if a.table_only else 'SPU_DUMMY'}"], env=env)
    rc |= r.returncode
sys.exit(rc)
