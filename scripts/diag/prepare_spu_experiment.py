"""Snapshot the installed runtime into scratch; never change a live runtime/build."""
import os
import argparse
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'scratch' / 'spu-codegen'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,default=OUT)
    args=parser.parse_args()
    out=args.output.resolve()
    source = Path(os.environ.get('PS3RECOMP_DIR', ROOT.parent / 'ps3recomp-tourline5')).resolve()
    out.mkdir(parents=True, exist_ok=True)
    for name in ('baseline','candidate'):
        if (out/name).exists():
            raise SystemExit(f'Refusing to overwrite {out/name}')
    for name in ('baseline', 'candidate'):
        dest = out / name
        dest.mkdir()
        # Copy tracked and untracked runtime sources, excluding build/cache data.
        paths = subprocess.check_output(['git', '-C', str(source), 'ls-files',
                                         '--cached', '--others', '--exclude-standard', '-z'])
        for raw in paths.split(b'\0'):
            if not raw:
                continue
            relative = Path(os.fsdecode(raw))
            src = source / relative
            if src.is_file():
                target = dest / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(src, target)
        print(dest, flush=True)
    (out / 'runtime-before.patch').write_bytes(subprocess.check_output(
        ['git', '-C', str(source), 'diff', '--binary', 'HEAD']))
    patch=ROOT/'patches/incremental/gt5-spu-codegen-experiment.patch'
    apply=['git','apply','--unsafe-paths',f'--directory={(out/"candidate").as_posix()}']
    subprocess.run(apply+['--check',str(patch)],cwd=ROOT,check=True)
    subprocess.run(apply+[str(patch)],cwd=ROOT,check=True)
    if not (out/'candidate/tools/spu_regcache_lazy.py').exists():
        raise SystemExit('Candidate patch was not applied')


if __name__ == '__main__':
    main()
