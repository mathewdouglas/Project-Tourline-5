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
    args = ap.parse_args()
    if not 1 <= args.jobs <= 8:
        raise ValueError('--jobs must be 1..8')
    work = args.work.resolve()
    private = args.private.resolve()
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

    run('cmd', '/d', '/c', r'scripts\build.cmd', '-DGT5P_TARGET=BCUS98114-v104', cwd=src, env=env)
    out = work / 'out' / f'project-tourline-5-v{version}-windows-x64.zip'
    run(py, 'scripts/release/package_player.py', '--build-dir', src / 'build', '--elf', src / 'input/EMAIN.ELF',
        '--runtime', work / 'ps3recomp', '--third-party', src / 'third_party', '--vc-runtime-dir', vc,
        '--output', out, cwd=src, env=env)
    print(f'Player ZIP: {out}  (commit {commit})')


if __name__ == '__main__':
    main()
