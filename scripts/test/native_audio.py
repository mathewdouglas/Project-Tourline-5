"""Native fast blocks of an audio image (audio_native_hooks in config/targets.json: the mixer
jD58063CD, src/gt5p_native_mixer.c; the EQ jEB79FDFC, src/gt5p_native_eq.c) against its lifted
code, on captured real states: every register and the whole local store must match, and the time per
state is printed for both.

Capture first (game-derived data: keep it in scratch/), with the hooks of audio_native_hooks:
    .venv\\Scripts\\python scripts\\diag\\spu_capture_patch.py jD58063CD 4720 4C80 44CC 4E58 1908
    (build) then a race with GT5P_NATIVE_MIXER=0 GT5P_SPU_CAPTURE=scratch\\pv\\mixcap.bin
    .venv\\Scripts\\python scripts\\diag\\spu_capture_patch.py --restore jD58063CD
Then:
    .venv\\Scripts\\python scripts\\test\\native_mixer.py [--states scratch\\pv\\mixcap.bin] [--reps 9]
Both variants are built like the FMA copy of the image the game runs (-msse4.1 -mfma, the audio
arithmetic header force-included), through scripts/test/spu_loop_bench.py.
"""
from pathlib import Path
import argparse, json, subprocess, sys, tempfile

root = Path(__file__).resolve().parents[2]
ap = argparse.ArgumentParser()
ap.add_argument("--states", default=str(root / "scratch" / "pv" / "mixcap.bin"))
ap.add_argument("--reps", type=int, default=9)
ap.add_argument("--image", default="jD58063CD")
a = ap.parse_args()
sys.path.insert(0, str(root / "scripts"))
import target
image, native_src, functions = next(h for h in target.TARGET["audio_native_hooks"] if h[0] == a.image)
lifted = root / "generated" / "spu_lifted" / image / "spu_recomp.c"
text = lifted.read_text(encoding="utf-8")
if "gt5p_spu_capture" in text:
    sys.exit("the lifted file still has the capture hook: run spu_capture_patch.py --restore")
for fn in functions:
    d = f"void gt5p_{image}_spu_func_{fn}(spu_context* ctx) {{"
    if text.count(d) != 1:
        sys.exit(f"no lifted function {fn} in {lifted}")
    text = text.replace(d, f"void gt5p_{image}_spu_func_{fn}_orig(spu_context* ctx) {{")
folder = Path(tempfile.mkdtemp(prefix="native-audio-", dir=root / "scratch"))
hooked = folder / "hooked.c"
hooked.write_text(text + f'\n#include "{(root / native_src).as_posix()}"\n', encoding="utf-8")
common = f"-mfma,/FI{(root / 'src' / 'gt5p_spu_audio_math.h').as_posix()},/DSPU_CODEGEN_HELPERS"
# The ADPCM decoder is a loop over two lifted functions (0x1908 -> 0x1A2C -> 0x1908): both
# variants follow it to its exit, so they do the same blocks.
import os
env = dict(os.environ, BENCH_CHAIN="1908,1A2C" if image == "jD58063CD" else "")
r = subprocess.run([sys.executable, str(root / "scripts" / "test" / "spu_loop_bench.py"),
                    "--image", image, "--states", a.states, "--reps", str(a.reps), "--block",
                    "--variant", f"lifted={common}",
                    "--variant", f"native={common},/DNM_PREFIX=gt5p_{image}_spu_func_,/DNS_PREFIX=gt5p_{image}_spu_func_,source={hooked}"], env=env)
sys.exit(r.returncode)
