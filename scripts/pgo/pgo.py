"""Profile-guided (PGO) builds of gt5.exe with clang-cl's IR instrumentation.

They live beside the normal build/, in build_pgo_gen/ and build_pgo_use/,
configured like build/ (target, runtime, GT5P_* options) plus a compiler
launcher (scripts/pgo/pgocc.py) that adds the profile flags. build/ never
changes. A profile comes from running the game, so it is derived data like the
lift: it stays in scratch/pgo/ and is never committed.

    pgo.py gen [--jobs 2]        instrumented build: build_pgo_gen/gt5.exe
    pgo.py play [--runs 1] [NAME=VALUE ...]
                                 the High Speed Ring race route (gt5boot.ps1
                                 -Route race -FrameTest: boot, menus, car list,
                                 loading, 60 s of racing) on the instrumented
                                 build; counts go to scratch/pgo/profraw/ while
                                 it runs (continuous mode), so the scripted kill
                                 loses nothing
    pgo.py merge                 scratch/pgo/profraw/*.profraw -> scratch/pgo/gt5.profdata
    pgo.py use [--jobs 4]        optimised build: build_pgo_use/gt5.exe
    pgo.py ab [--rounds 4]       frame tests alternating build/gt5.exe and
                                 build_pgo_use/gt5.exe (after bringing build/
                                 up to date), with a paired summary
    pgo.py clean-gen             delete build_pgo_gen/ (about 3 GB) once the
                                 profile is merged

A profile fits the sources and the lift it was made from. After a re-lift or a
source change, a stale profile still builds (changed functions just get no
counts); `use` prints how many functions no longer match.
"""
import argparse
import hashlib
import os
import re
import shutil
import statistics
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PGO = ROOT / 'scratch' / 'pgo'
NORMAL = ROOT / 'build'
VCVARS = r'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat'
LLVM = Path(r'C:\Program Files\LLVM\bin')
CACHE_KEYS = re.compile(r'^(GT5P_[A-Z0-9_]+|PS3RECOMP_DIR|PS3RECOMP_RSX_VULKAN|PT5_[A-Z0-9_]+|'
                        r'CMAKE_BUILD_TYPE|CMAKE_(C|CXX)_COMPILER):(BOOL|STRING|PATH|FILEPATH|UNINITIALIZED)=(.*)$')


def say(msg):
    print('[pgo]', msg, flush=True)


def normal_settings():
    cache = NORMAL / 'CMakeCache.txt'
    if not cache.is_file():
        sys.exit(f'[pgo] no {cache}: build the normal way first (scripts\\build.cmd -DGT5P_TARGET=...)')
    out = []
    for line in cache.read_text(errors='replace').splitlines():
        m = CACHE_KEYS.match(line)
        if m:
            out.append(f'-D{m.group(1)}={m.group(4)}')
    return out


def run_bat(path, lines, log):
    path.write_text('@echo off\r\n' + '\r\n'.join(lines) + '\r\n')
    with open(log, 'w', encoding='utf-8', errors='replace') as f:
        return subprocess.call(['cmd', '/c', str(path)], stdout=f, stderr=subprocess.STDOUT)


def build(mode, arg, jobs):
    d = ROOT / f'build_pgo_{mode}'
    d.mkdir(exist_ok=True)
    launcher = f'{sys.executable};{ROOT / "scripts/pgo/pgocc.py"};{mode};{arg}'.replace('\\', '/')
    defs = ' '.join(f'"{x}"' for x in normal_settings())
    lines = [f'set "PATH=C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer;%PATH%"',
             f'call "{VCVARS}" >nul', 'if errorlevel 1 exit /b 1']
    last_arg = (d / 'pgo.arg').read_text() if (d / 'pgo.arg').is_file() else ''
    if not (d / 'build.ninja').is_file() or last_arg != arg:
        # The launcher's arguments are in every compile command, so a new
        # profile path recompiles everything once configure sets them.
        lines.append(f'cmake -S "{ROOT}" -B "{d}" -G Ninja {defs} '
                     f'"-DCMAKE_C_COMPILER_LAUNCHER={launcher}" "-DCMAKE_CXX_COMPILER_LAUNCHER={launcher}" || exit /b 1')
    if mode == 'gen':
        # The instrumented objects ask for clang's profile runtime, built /MT
        # (libcmt); the game is /MD.
        res = subprocess.run([str(LLVM / 'clang-cl.exe'), '--print-resource-dir'], capture_output=True,
                             text=True).stdout.strip()
        lib = Path(res) / 'lib' / 'windows'
        if not (lib / 'clang_rt.profile-x86_64.lib').is_file():
            sys.exit(f'[pgo] no clang_rt.profile-x86_64.lib under {lib}')
        lines.append(f'set "_LINK_=/LIBPATH:"{lib}" /NODEFAULTLIB:libcmt.lib"')
    else:
        lines.append('set _LINK_=')
    lines.append(f'cmake --build "{d}" --target gt5p --parallel {jobs} || exit /b 1')
    say(f'building {d.name} ({jobs} jobs); log {d / "pgo_build.log"}')
    code = run_bat(d / 'pgo_build.bat', lines, d / 'pgo_build.log')
    if code:
        log = (d / 'pgo_build.log').read_text(errors='replace').splitlines()
        for l in [l for l in log if ' error' in l or 'error:' in l][:20] or log[-20:]:
            print('   ', l)
        sys.exit(f'[pgo] build in {d} failed')
    (d / 'pgo.arg').write_text(arg)
    exe = d / 'gt5.exe'
    if not exe.is_file():
        sys.exit(f'[pgo] no {exe}')
    say(f'built {exe}')
    return d


def gt5boot(tag, exe, extra=(), frame_test=True, settle=40):
    args = ['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', str(ROOT / 'scripts/test/gt5boot.ps1'),
            '-Tag', tag, '-Route', 'race', '-Settle', str(settle), '-Exe', str(exe)]
    if frame_test:
        args.append('-FrameTest')
    args += list(extra)
    out = subprocess.run(args, cwd=ROOT, capture_output=True, text=True, errors='replace').stdout
    (ROOT / 'scratch/pv' / f'{tag}.run.txt').write_text(out)
    return out


def cmd_gen(a):
    (PGO / 'profraw').mkdir(parents=True, exist_ok=True)
    build('gen', str(PGO / 'profraw').replace('\\', '/'), a.jobs)
    say('next: scripts/pgo/pgo.py play')


def cmd_play(a):
    exe = ROOT / 'build_pgo_gen/gt5.exe'
    if not exe.is_file():
        sys.exit('[pgo] no build_pgo_gen/gt5.exe: run gen first')
    raw = PGO / 'profraw'
    raw.mkdir(parents=True, exist_ok=True)
    # %p: a file per run; %c: continuous mode, counts written while it runs.
    env = [f'LLVM_PROFILE_FILE={raw.as_posix()}/gt5-%p%c.profraw'] + a.vars
    for i in range(a.runs):
        out = gt5boot(f'pgo-play{i + 1}', exe, env, settle=a.settle)
        say(f'run {i + 1}: ' + ' | '.join(l for l in out.splitlines() if 'FrameTest' in l or 'RaceStart' in l))
    files = sorted(raw.glob('*.profraw'))
    say(f'{len(files)} profile file(s), {sum(f.stat().st_size for f in files) >> 20} MB; next: pgo.py merge')


def cmd_merge(a):
    files = sorted((PGO / 'profraw').glob('*.profraw'))
    if not files:
        sys.exit('[pgo] no profiles in scratch/pgo/profraw: run play first')
    out = PGO / 'gt5.profdata'
    subprocess.run([str(LLVM / 'llvm-profdata.exe'), 'merge', '-o', str(out)] + [str(f) for f in files], check=True)
    say(f'merged {len(files)} run(s) into {out} ({out.stat().st_size >> 20} MB); next: pgo.py use')


def profile_fit(log):
    """(changed functions that lost counts, functions the profile has no record of) in a build log."""
    lines = log.splitlines()
    stale = sum(1 for l in lines if 'hash mismatch' in l and 'up to 0 count discarded' not in l)
    missing = sum(1 for l in lines if 'no profile data available for function' in l)
    return stale, missing


def cmd_use(a):
    prof = Path(a.profile) if a.profile else PGO / 'gt5.profdata'
    if not prof.is_file():
        sys.exit(f'[pgo] no profile at {prof}: run merge first')
    # A copy named by content: a new profile is a new path, so ninja rebuilds.
    digest = hashlib.sha256(prof.read_bytes()).hexdigest()[:12]
    use = PGO / 'use' / f'gt5-{digest}.profdata'
    use.parent.mkdir(parents=True, exist_ok=True)
    if not use.is_file():
        shutil.copyfile(prof, use)
    d = build('use', str(use).replace('\\', '/'), a.jobs)
    log = (d / 'pgo_build.log').read_text(errors='replace')
    stale, missing = profile_fit(log)
    say(f'in what this build compiled: {stale} function(s) the profile no longer fits, '
        f'{missing} it has no record of')
    say('next: scripts/pgo/pgo.py ab')


FRAMES = re.compile(r'frames (\d+) over [\d.]+ s: ([\d.]+) FPS; interval median ([\d.]+) ms, p99 ([\d.]+) ms.*?'
                    r'late \(>20 ms\) ([\d.]+)%; walk ([\d.]+) ms/frame')


def cmd_ab(a):
    pgo_exe = ROOT / 'build_pgo_use/gt5.exe'
    if not pgo_exe.is_file():
        sys.exit('[pgo] no build_pgo_use/gt5.exe: run use first')
    say('bringing build/ up to date (same sources as the PGO build)')
    if run_bat(PGO / 'normal_build.bat', [f'set "CMAKE_BUILD_PARALLEL_LEVEL={a.jobs}"',
               f'call "{ROOT / "scripts/build.cmd"}" || exit /b 1'], PGO / 'normal_build.log'):
        sys.exit(f'[pgo] normal build failed: {PGO / "normal_build.log"}')
    sides = {'A': NORMAL / 'gt5.exe', 'B': pgo_exe}
    res = {'A': [], 'B': []}
    for r in range(a.rounds):
        order = 'AB' if r % 2 == 0 else 'BA'   # alternate, so drift falls on both sides
        for s in order:
            tag = f'pgoab-{s}{r + 1}'
            gt5boot(tag, sides[s], settle=a.settle)
            txt = ROOT / 'scratch/pv' / f'{tag}.frames.txt'
            m = FRAMES.search(txt.read_text()) if txt.is_file() else None
            if not m:
                say(f'{tag}: no frame test result (see scratch/pv/{tag}.run.txt)')
                res[s].append(None)
                continue
            v = dict(fps=float(m[2]), median=float(m[3]), p99=float(m[4]), late=float(m[5]), walk=float(m[6]))
            res[s].append(v)
            say(f'{tag} ({"normal" if s == "A" else "PGO"}): {v["fps"]:.1f} FPS, late {v["late"]:.1f}%, '
                f'p99 {v["p99"]:.1f} ms, walk {v["walk"]:.2f} ms')
    pairs = [(x, y) for x, y in zip(res['A'], res['B']) if x and y]
    if not pairs:
        sys.exit('[pgo] no complete rounds')
    lines = ['| | normal | PGO | PGO - normal (mean of rounds) | rounds PGO better |', '|---|---|---|---|---|']
    for k, better in (('fps', 1), ('late', -1), ('p99', -1), ('walk', -1)):
        A = [p[0][k] for p in pairs]; B = [p[1][k] for p in pairs]
        diff = [b - x for x, b in zip(A, B)]
        wins = sum(1 for d in diff if d * better > 0)
        lines.append(f'| {k} | {statistics.mean(A):.2f} | {statistics.mean(B):.2f} | '
                     f'{statistics.mean(diff):+.2f} ({statistics.mean(diff) / statistics.mean(A) * 100:+.1f}%) | '
                     f'{wins} of {len(pairs)} |')
    table = '\n'.join(lines)
    (PGO / 'ab.txt').write_text(table + '\n')
    print(table)


def cmd_clean_gen(a):
    d = ROOT / 'build_pgo_gen'
    if d.is_dir():
        shutil.rmtree(d)
        say(f'removed {d}')


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='cmd', required=True)
    g = sub.add_parser('gen'); g.add_argument('--jobs', type=int, default=2); g.set_defaults(fn=cmd_gen)
    g = sub.add_parser('play'); g.add_argument('--runs', type=int, default=1); g.add_argument('--settle', type=int, default=60)
    g.add_argument('vars', nargs='*', help='NAME=VALUE for the game'); g.set_defaults(fn=cmd_play)
    g = sub.add_parser('merge'); g.set_defaults(fn=cmd_merge)
    g = sub.add_parser('use'); g.add_argument('--jobs', type=int, default=4); g.add_argument('--profile')
    g.set_defaults(fn=cmd_use)
    g = sub.add_parser('ab'); g.add_argument('--rounds', type=int, default=4); g.add_argument('--settle', type=int, default=40)
    g.add_argument('--jobs', type=int, default=4); g.set_defaults(fn=cmd_ab)
    g = sub.add_parser('clean-gen'); g.set_defaults(fn=cmd_clean_gen)
    a = p.parse_args()
    a.fn(a)


if __name__ == '__main__':
    main()
