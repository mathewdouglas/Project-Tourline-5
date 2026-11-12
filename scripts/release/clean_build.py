"""Build the player ZIP from a fresh clone of a release tag, with only explicit private inputs.

Nothing from a development checkout's ignored folders (generated/, build/,
third_party/, shadercache/, gamedata/) is reused. The work folder gets:

  src/        a fresh clone of the repository at --ref (default v<version>)
  ps3recomp/  the pinned runtime plus this commit's patch (scripts/setup_runtime.py)
  out/        the player ZIP

Private inputs, copied from --private (a folder laid out like a checkout):
input/EMAIN.ELF and the captured SPU images under analysis/spu/dump and
analysis/spu/taskdump. Third-party DLLs come from their pinned downloads
(get_dxc.py, get_ffmpeg.ps1, get_sdl2.ps1). The lift and build follow the
README. The run fails if the lift changes a tracked file, so the release is
reproducible from the tag.

    python scripts/release/clean_build.py --private C:/path/to/dev/checkout --work C:/pt5-release/0.1.0

With --profile, the build is profile-guided (docs/pgo.md): every compile goes
through the tag's scripts/pgo/pgocc.py with that profile, which is one more
private input. Make it from the same sources as the tag (pgo.py gen, play,
merge in a development checkout at the tag). The build log is checked: the run
fails if any compiled function lost its counts or has no record in the profile
(--profile-max-stale to allow some), and release.ini records the profile's
SHA-256.

    python scripts/release/clean_build.py --private C:/dev --work C:/pt5-release/0.1.4 \
        --profile C:/dev/scratch/pgo/gt5.profdata

It never launches the game, pushes, tags or publishes.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parents[2]
VC_REDIST = Path('C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Redist/MSVC')
CLANG = 'C:/Program Files/LLVM/bin/clang-cl.exe'


def run(*args, cwd, env=None):
    print('+', ' '.join(str(a) for a in args), flush=True)
    subprocess.run([str(a) for a in args], cwd=cwd, env=env, check=True)


def copy(source, dest):
    source = Path(source)
    if not source.is_file() or source.is_symlink():
        raise ValueError(f'Missing private input or unexpected link: {source}')
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, dest)


def file_sha(path):
    with open(path, 'rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def tree_sha(folder):
    h = hashlib.sha256()
    for p in sorted(folder.rglob('*')):
        if p.is_file():
            h.update(p.relative_to(folder).as_posix().encode() + b'\0')
            h.update(file_sha(p).encode())
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--private', required=True, type=Path)
    ap.add_argument('--work', required=True, type=Path)
    ap.add_argument('--repo', default='https://github.com/mathewdouglas/Project-Tourline-5.git')
    ap.add_argument('--ref', help='tag or commit (default: v<version in CMakeLists.txt>)')
    ap.add_argument('--jobs', type=int, default=4)
    ap.add_argument('--vc-runtime-dir', type=Path)
    ap.add_argument('--compare-generated', type=Path,
                    help='optional generated/ of an earlier lift: report whether this lift is byte-identical')
    ap.add_argument('--profile', type=Path,
                    help='a merged PGO profile (.profdata) made from the same sources: profile-guided build')
    ap.add_argument('--profile-max-stale', type=int, default=0,
                    help='compiled functions the profile may miss or no longer fit (default 0)')
    args = ap.parse_args()
    if not 1 <= args.jobs <= 8:
        raise ValueError('--jobs must be 1..8')
    work = args.work.resolve()
    private = args.private.resolve()
    profile = None
    if args.profile:
        profile = args.profile.resolve()
        if not profile.is_file() or profile.is_symlink():
            raise ValueError(f'Missing profile or unexpected link: {profile}')
        with profile.open('rb') as f:
            if f.read(8) != b'\xfflprofi\x81':
                raise ValueError(f'Not an indexed LLVM profile (llvm-profdata merge output): {profile}')
    if work.exists() and any(work.iterdir()):
        raise ValueError(f'Work folder must be new or empty: {work}')
    if work == private or private in work.parents or work in private.parents or HERE in work.parents or work in HERE.parents:
        raise ValueError('Work folder must be separate from the private inputs and this checkout')
    version = re.search(r'VERSION\s+(\d+\.\d+\.\d+)', (HERE / 'CMakeLists.txt').read_text(encoding='utf-8')).group(1)
    ref = args.ref or f'v{version}'
    vc = args.vc_runtime_dir
    if vc is None:
        found = sorted(VC_REDIST.glob('14.*/x64/Microsoft.VC143.CRT'))
        if not found:
            raise ValueError('Pass --vc-runtime-dir: no Visual Studio 2022 x64 CRT redistributable found')
        vc = found[-1]
    work.mkdir(parents=True, exist_ok=True)
    src = work / 'src'
    run('git', 'clone', '--quiet', args.repo, src, cwd=work)
    run('git', '-c', 'advice.detachedHead=false', 'checkout', '--quiet', ref, cwd=src)
    commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=src, text=True).strip()
    print(f'Building {ref} = {commit}', flush=True)
    py = sys.executable
    # Diagnostic switches of a developer shell must not change the lift or build.
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(('PPU_', 'SPU_', 'GT5P_', 'RSX_', 'GCM_', 'PS3_', 'PAD_'))}
    env['PS3RECOMP_DIR'] = str(work / 'ps3recomp')
    env['GT5P_TARGET'] = 'BCUS98114-v104'
    env['CMAKE_BUILD_PARALLEL_LEVEL'] = str(args.jobs)
    run(py, 'scripts/check_no_game_files.py', '--tracked', cwd=src, env=env)
    run(py, 'scripts/setup_runtime.py', '--path', work / 'ps3recomp', cwd=src, env=env)

    copy(private / 'input/EMAIN.ELF', src / 'input/EMAIN.ELF')
    (src / 'input/target').write_text('BCUS98114-v104\n', encoding='ascii')
    for sub in ('dump', 'taskdump'):
        folder = private / 'analysis/spu' / sub
        for blob in sorted(folder.glob('*')):
            if blob.is_file():
                copy(blob, src / 'analysis/spu' / sub / blob.name)
    run(py, 'scripts/get_dxc.py', cwd=src, env=env)
    for script in ('get_ffmpeg.ps1', 'get_sdl2.ps1'):
        run('powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', f'scripts/{script}', cwd=src, env=env)

    tools = work / 'ps3recomp/tools'
    code_end = json.loads((src / 'config/targets.json').read_text())['BCUS98114-v104']['code_end']
    run(py, 'scripts/jumptables.py', '--write', cwd=src, env=env)
    run(py, 'scripts/code_labels.py', '--write', cwd=src, env=env)
    run(py, tools / 'find_functions.py', 'input/EMAIN.ELF', '--json', '--seed-json',
        'config/extra_seeds_bcus98114_v104.json', '--output', 'analysis/functions.json', cwd=src, env=env)
    functions = json.loads((src / 'analysis/functions.json').read_text())
    end = int(code_end, 16)
    (src / 'analysis/functions_code.json').write_text(json.dumps([f for f in functions if int(f['start'], 16) < end]))
    run(py, 'scripts/gen_hle_stubs.py', 'input/EMAIN.ELF', 'analysis/hle_stubs.json', cwd=src, env=env)
    run(py, tools / 'ppu_lifter.py', 'input/EMAIN.ELF', '--functions', 'analysis/functions_code.json',
        '--output', 'generated', '--code-end', code_end, '--header-name', 'ppu_recomp.h',
        '--source-name', 'ppu_recomp.c', '--hle-stubs', 'analysis/hle_stubs.json', cwd=src, env=env)
    run(py, 'scripts/setjmp_patch.py', cwd=src, env=env)
    run(py, 'scripts/lift_spu_jobs.py', cwd=src, env=env)
    for script in ('spin_patch.py', 'ui_mask_patch.py', 'spu_native_patch.py', 'spu_regpromo.py', 'spu_region.py'):
        run(py, f'scripts/{script}', cwd=src, env=env)
    run(py, tools / 'gen_hle_nids.py', '--all', '--out', 'src/gen/ppu_hle_nids.cpp', cwd=src, env=env)
    changed = subprocess.check_output(['git', 'status', '--porcelain', '--untracked-files=no'], cwd=src, text=True)
    if changed.strip():
        raise ValueError('The lift changed tracked files; the tag does not reproduce:\n' + changed)
    if args.compare_generated:
        same = tree_sha(src / 'generated') == tree_sha(args.compare_generated.resolve())
        print(f'generated/ identical to {args.compare_generated}: {same}', flush=True)

    build = ['cmd', '/d', '/c', r'scripts\build.cmd', '-DGT5P_TARGET=BCUS98114-v104']
    pgo_args = []
    if profile:
        pgocc = src / 'scripts/pgo/pgocc.py'
        if not pgocc.is_file():
            raise ValueError(f'{ref} has no scripts/pgo/pgocc.py: it predates profile-guided builds')
        # Outside src/, so the tree stays as the tag has it; named by content.
        used = work / 'pgo' / f'gt5-{file_sha(profile)[:12]}.profdata'
        copy(profile, used)
        launcher = ';'.join(p.as_posix() for p in (Path(py), pgocc)) + ';use;' + used.as_posix()
        build += [f'-DCMAKE_C_COMPILER_LAUNCHER={launcher}', f'-DCMAKE_CXX_COMPILER_LAUNCHER={launcher}']
        pgo_args = ['--pgo-profile', used]
    build_log(build, src, env, work / 'build.log')
    if profile:
        log = (work / 'build.log').read_text(encoding='utf-8', errors='replace').splitlines()
        stale = sum(1 for l in log if 'hash mismatch' in l and 'up to 0 count discarded' not in l)
        missing = sum(1 for l in log if 'no profile data available for function' in l)
        print(f'Profile {profile.name}: {stale} compiled function(s) it no longer fits, '
              f'{missing} it has no record of (details in {work / "build.log"})', flush=True)
        if stale + missing > args.profile_max_stale:
            raise ValueError('The profile does not fit these sources: make it from the tag '
                             '(docs/pgo.md), or pass --profile-max-stale')
    out = work / 'out' / f'project-tourline-5-v{version}-windows-x64.zip'
    run(py, 'scripts/release/package_player.py', '--build-dir', src / 'build', '--elf', src / 'input/EMAIN.ELF',
        '--runtime', work / 'ps3recomp', '--third-party', src / 'third_party', '--vc-runtime-dir', vc,
        '--output', out, *pgo_args, cwd=src, env=env)
    print(f'Player ZIP: {out}  (commit {commit}{", profile-guided" if profile else ""})')


def build_log(args, cwd, env, log):
    """Run the build, keeping its whole output in log; per-function profile warnings stay out of the console."""
    print('+', ' '.join(str(a) for a in args), flush=True)
    with open(log, 'w', encoding='utf-8', errors='replace') as f:
        p = subprocess.Popen([str(a) for a in args], cwd=cwd, env=env, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True, errors='replace')
        for line in p.stdout:
            f.write(line)
            if '[-Wbackend-plugin]' not in line:
                print(line, end='', flush=True)
        if p.wait():
            raise subprocess.CalledProcessError(p.returncode, args)


if __name__ == '__main__':
    main()
