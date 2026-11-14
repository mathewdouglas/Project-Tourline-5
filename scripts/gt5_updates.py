"""GT5 update packages: check a player's folder and install the chain.

The 2.11 target needs every update from 01.05 to 02.11 in order: each
package's PARAM.SFO names the version it applies to (TARGET_APP_VER).
config/gt5-updates.json pins each package's file name, size and SHA-1 (from
Sony's public update list). Installing a package writes its files into the
game folder (dev_hdd0/game/BCUS98114), which is what this does, with the
runtime's tools/pkg_extract.py; RPCS3 is not needed.

  gt5_updates.py check   <folder> [--to 02.11]
  gt5_updates.py install <folder> <game dir> [--to 02.11] [--base <01.04 game dir>]

check: every package present, its size and SHA-1 as pinned, its PARAM.SFO
APP_VER as named and TARGET_APP_VER equal to the previous version.
install: check, then (with --base) copy the 01.04 game folder's package
files first, then extract each package in order over the game folder.
Writes <game dir>/../BCUS98114.updates.json with what was installed.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
cfg = json.loads((root / "config/gt5-updates.json").read_text())
runtime = Path(os.environ.get("PS3RECOMP_DIR", root.parent / "ps3recomp-tourline5"))
extractor = runtime / "tools/pkg_extract.py"


def chain(to):
    out = []
    for p in cfg["packages"]:
        out.append(p)
        if p["version"] == to:
            return out
    raise SystemExit(f"version {to} is not in config/gt5-updates.json")


def sfo(data):
    _, _, kt, dt, n = struct.unpack_from("<4sIIII", data, 0)
    r = {}
    for i in range(n):
        ko, fmt, ln, _, do = struct.unpack_from("<HHIII", data, 20 + 16 * i)
        k = data[kt + ko:data.index(b"\0", kt + ko)].decode()
        v = data[dt + do:dt + do + ln]
        r[k] = struct.unpack("<I", v)[0] if fmt == 0x404 else v.rstrip(b"\0").decode(errors="replace")
    return r


def extract(pkg, out, only=None):
    args = [sys.executable, "-I", str(extractor), str(pkg), str(out)] + (["--only", only] if only else [])
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode:
        raise SystemExit(f"pkg_extract failed on {pkg.name}:\n{r.stdout[-2000:]}{r.stderr[-2000:]}")
    return r.stdout


def check(folder, to):
    folder = Path(folder)
    prev, bad = cfg["base_version"], 0
    for p in chain(to):
        f = folder / p["file"]
        if not f.is_file():
            print(f"  {p['version']}: missing {p['file']}"); bad += 1; prev = p["version"]; continue
        if f.stat().st_size != p["bytes"]:
            print(f"  {p['version']}: size {f.stat().st_size}, expected {p['bytes']}"); bad += 1
        # Sony's SHA-1 covers the package without its last 32 bytes, which
        # hold that same digest followed by 12 zero bytes.
        h, left = hashlib.sha1(), f.stat().st_size - 32
        with open(f, "rb") as fh:
            while left > 0:
                b = fh.read(min(1 << 22, left))
                if not b: break
                h.update(b); left -= len(b)
        if h.hexdigest() != p["sha1"]:
            print(f"  {p['version']}: SHA-1 differs"); bad += 1
        with tempfile.TemporaryDirectory(prefix="gt5upd-") as t:
            extract(f, t, "PARAM.SFO")
            s = sfo((Path(t) / "PARAM.SFO").read_bytes())
        # 01.05, the first update after the disc's 01.04 package, names no
        # target; every later one names the version before it.
        want = None if prev == cfg["base_version"] else prev
        ok = s.get("APP_VER") == p["version"] and s.get("TARGET_APP_VER") == want
        if not ok:
            bad += 1
        print(f"  {p['version']}: APP_VER {s.get('APP_VER')}, TARGET_APP_VER {s.get('TARGET_APP_VER')}"
              f"{'' if ok else '  <-- expected ' + str(want)}")
        prev = p["version"]
    print(f"{len(chain(to))} packages to {to}: {'ok' if not bad else str(bad) + ' problem(s)'}")
    return bad == 0


def install(folder, game, to, base):
    if not check(folder, to):
        raise SystemExit("not installing: fix the packages above first")
    game = Path(game)
    game.mkdir(parents=True, exist_ok=True)
    if base:
        n = 0
        for dp, _, fn in os.walk(base):
            for f in fn:
                s = Path(dp) / f
                d = game / s.relative_to(base)
                d.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(s, d); n += 1
        print(f"base: {n} files from {base}")
    log = []
    for p in chain(to):
        out = extract(Path(folder) / p["file"], game)
        files = [l for l in out.splitlines() if l.startswith("  ")]
        log.append({"version": p["version"], "files": len(files)})
        print(f"  {p['version']}: {len(files)} files")
    total = sum(f.stat().st_size for f in game.rglob("*") if f.is_file())
    count = sum(1 for f in game.rglob("*") if f.is_file())
    (game.parent / (game.name + ".updates.json")).write_text(json.dumps(
        {"to": to, "packages": log, "files": count, "bytes": total}, indent=2) + "\n")
    print(f"installed to {to}: {count} files, {total / 2**30:.2f} GiB in {game}")


ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
ap.add_argument("cmd", choices=["check", "install"])
ap.add_argument("folder")
ap.add_argument("game", nargs="?")
ap.add_argument("--to", default="02.11")
ap.add_argument("--base")
a = ap.parse_args()
if a.cmd == "check":
    sys.exit(0 if check(a.folder, a.to) else 1)
if not a.game:
    ap.error("install needs a game dir")
install(a.folder, a.game, a.to, a.base)
