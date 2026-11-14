"""Offline benchmark of one lifted SPU function from captured real states.

Capture states first (game-derived data: keep them in scratch/):
    .venv\\Scripts\\python scripts\\diag\\spu_capture_patch.py b01697100 7DC8
    (private build) then a run with GT5P_SPU_CAPTURE=scratch\\pv\\loopcap.bin
    .venv\\Scripts\\python scripts\\diag\\spu_capture_patch.py --restore b01697100

Then build the lifted body once per variant (compile definitions, and
optionally another runtime), replay every state through it, check that every
variant ends in exactly the same registers and local store as the first,
and print median times:
    python scripts/test/spu_loop_bench.py [--states F] [--image b01697100] [--reps 9]
        [--variant NAME=-DFLAG,-DFLAG2] ...
Default variants: "base" only. Example: --variant old=-DSPU_HELPERS_LEGACY --variant new=
The first variant is the reference. Each build uses the production flags of
lifted SPU code (/O2 -msse4.1, SPU_LS_NO_PROBES). Needs Visual Studio 2022
and LLVM (clang-cl). Builds go to an ignored scratch folder.
Exit status 1 if any variant's results differ from the reference.
"""
from pathlib import Path
import argparse, os, re, subprocess, sys, tempfile

root = Path(__file__).resolve().parents[2]
ap = argparse.ArgumentParser()
ap.add_argument("--states", default=str(root / "scratch" / "pv" / "loopcap.bin"))
ap.add_argument("--image", default="b01697100")
ap.add_argument("--reps", type=int, default=9)
ap.add_argument("--runtime", default=os.environ.get("PS3RECOMP_DIR", str(root.parent / "ps3recomp-tourline5")))
ap.add_argument("--ranges", default="", help="entry:lo:hi,... (hex): an indirect branch out of the range ends that routine's run")
ap.add_argument("--count", action="store_true", help="count entries of every lifted function (first variant only)")
ap.add_argument("--strip-native", action="store_true", help="replay lifted fallbacks instead of external native hooks")
ap.add_argument("--block", action="store_true", help="stop at the captured block's first external transfer")
ap.add_argument("--variant", action="append", default=[], help="NAME=-Dflag,... ; runtime=<dir>, rp=a:b, region=a:b+c:d (regions of functions, hex)")
a = ap.parse_args()
variants = a.variant or ["base="]
lifted = root / "generated" / "spu_lifted" / a.image / "spu_recomp.c"
if "gt5p_spu_capture" in lifted.read_text(encoding="latin-1"):
    sys.exit("the lifted file still has the capture hook: run spu_capture_patch.py --restore")
folder = Path(tempfile.mkdtemp(prefix="spu-loop-bench-", dir=root / "scratch"))
if a.count:
    src = lifted.read_text(encoding="latin-1")
    src = re.sub(r"^(void gt5p_%s_spu_func_([0-9A-F]{8})\(spu_context\* ctx\) \{\r?\n)" % a.image,
                 lambda m: m.group(1) + "        bench_count(0x%su);\n" % m.group(2), src, flags=re.M)
    lifted = folder / "counted.c"
    lifted.write_text("void bench_count(unsigned);\n" + src, encoding="latin-1")
    variants = variants[:1]
VC = ('@echo off\nset "PATH=C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer;%PATH%"\n'
      'call "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\n')
results = []
for v in variants:
    name, _, flags = v.partition("=")
    defs, rt, rp, region, allr = [], a.runtime, [], [], True
    source_override = None
    for f in filter(None, flags.split(",")):
        if f.startswith("runtime="): rt = f[len("runtime="):]
        elif f.startswith("source="): source_override = Path(f[len("source="):]).resolve()
        elif f.startswith("rp="): rp = [int(x, 16) for x in f[3:].split(":")]
        elif f.startswith("region="): region = [[int(x, 16) for x in g.split(":")] for g in f[7:].split("+")]
        elif f.startswith("regionw="): region, allr = [[int(x, 16) for x in g.split(":")] for g in f[8:].split("+")], False
        else: defs.append(f)
    out = folder / name
    out.mkdir()
    src_v = source_override or lifted
    if a.strip_native:
        clean = "\n".join(l for l in src_v.read_text(encoding="utf-8").split("\n") if "/* spu_native_patch */" not in l)
        src_v = out / "without-native.c"
        src_v.write_text(clean, encoding="utf-8")
    if a.block:
        sys.path.insert(0,str(root/'scripts'))
        import spu_regpromo
        stopped="\n".join(spu_regpromo.stopify(l) for l in src_v.read_text(encoding='utf-8').split('\n'))
        src_v=out/'block.c'
        src_v.write_text(stopped,encoding='utf-8')
        defs.append('/DBENCH_BLOCK')
    if rp:
        # Register promotion (scripts/spu_regpromo.py) of these functions, on.
        sys.path.insert(0, str(root / "scripts"))
        import spu_regpromo
        text, report = spu_regpromo.patch_image(a.image, rp, src_v.read_text(encoding="utf-8"))
        print(f"{name}: " + "; ".join(report))
        src_v = out / "promoted.c"
        src_v.write_text(text, encoding="utf-8")
        defs.append("/DBENCH_RP")
    if region:
        # The loop's functions merged into one with registers in locals.
        sys.path.insert(0, str(root / "scripts"))
        import spu_region
        text, report = spu_region.patch_image(a.image, region, src_v.read_text(encoding="utf-8"), allr)
        print(f"{name}: " + "; ".join(report))
        src_v = out / "region.c"
        src_v.write_bytes(text.encode("utf-8"))
        defs.append("/DBENCH_REGION")
    cmd = ('"C:\\Program Files\\LLVM\\bin\\clang-cl.exe" /nologo /O2 /Ob2 /MD /std:c17 /W0 -msse4.1 '
           '/D_CRT_SECURE_NO_WARNINGS /DSPU_LS_NO_PROBES '
           f'/DBENCH_LIFTED="\\"{src_v.as_posix()}\\"" /DBENCH_REGISTER=gt5p_{a.image}_spu_recomp_register '
           + ("/DBENCH_COUNT " if a.count else "") + " ".join(defs) + f' /I"{rt}/runtime/spu" /I"{root / "generated" / "spu_lifted" / a.image}" /Fo"{out}/" /Fe"{out}/bench.exe" '
           f'"{root}/scripts/test/spu_loop_bench.c"\n')
    (out / "build.cmd").write_text(VC + cmd, encoding="ascii")
    b = subprocess.run(["cmd", "/c", str(out / "build.cmd")], capture_output=True, text=True)
    if b.returncode:
        sys.exit(f"{name}: build failed\n{b.stdout[-4000:]}{b.stderr[-2000:]}")
    r = subprocess.run([str(out / "bench.exe"), a.states, str(a.reps)] + ([a.ranges] if a.ranges else []),
                       capture_output=True, text=True, timeout=1800)
    if r.returncode:
        sys.exit(f"{name}: run failed\n{r.stdout}{r.stderr}")
    if a.count:
        print(r.stdout[r.stdout.find("counts"):])
    rows = re.findall(r"entry ([0-9A-F]+) calls +\d+: +([\d.]+) us  hash ([0-9A-F]{16})", r.stdout)
    times = [float(t) for _, t, _ in rows]
    hashes = [h for _, _, h in rows]
    entries = [e for e, _, _ in rows]
    results.append((name, times, hashes, entries))
    print(f"{name:12s} total {sum(times):10.1f} us over {len(times)} states")

ref = results[0]
bad = 0
for name, times, hashes, entries in results[1:]:
    diff = [i for i, (x, y) in enumerate(zip(ref[2], hashes)) if x != y]
    speed = sum(ref[1]) / sum(times) if sum(times) else 0
    print(f"{name:12s} vs {ref[0]}: {speed:5.2f}x, " + ("results identical" if not diff and len(hashes) == len(ref[2])
                                                       else f"RESULTS DIFFER in states {diff}"))
    for e in sorted(set(entries)):
        a0 = sum(t for t, x in zip(ref[1], ref[3]) if x == e); a1 = sum(t for t, x in zip(times, entries) if x == e)
        if len(set(entries)) > 1:
            print(f"    entry {e}: {a0:9.1f} -> {a1:9.1f} us ({a0 / a1 if a1 else 0:4.2f}x)")
    bad |= bool(diff) or len(hashes) != len(ref[2])
sys.exit(1 if bad else 0)
