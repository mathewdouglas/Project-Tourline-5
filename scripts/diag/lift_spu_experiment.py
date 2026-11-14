"""Re-lift into scratch using a candidate runtime, preserving active generated/.

Run at idle priority. Reuses the private executable/dumps read-only. All
intermediate binaries, boundaries, registry and generated sources stay private.
"""
import argparse
import ctypes
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'scripts'))
import lift_spu_jobs as lift
import spu_native_patch
import spu_regpromo
import spu_region
import target


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--name',required=True)
    ap.add_argument('--lazy',action='store_true')
    ap.add_argument('--cache-ls',action='store_true')
    ap.add_argument('--full-locals',action='store_true',help='localize rejected legacy-promotion blocks too; preserve measured regions')
    a = ap.parse_args()
    if (a.cache_ls or a.full_locals) and not a.lazy:
        ap.error('--cache-ls and --full-locals require --lazy')
    if Path(a.name).name != a.name:
        raise SystemExit('name must be a single path component')
    ctypes.windll.kernel32.SetPriorityClass(ctypes.windll.kernel32.GetCurrentProcess(),0x40)
    base = ROOT/'scratch/spu-codegen'
    dest = base/a.name
    if dest.exists():
        raise SystemExit(f'Refusing to overwrite {dest}')
    (dest/'analysis/spu').mkdir(parents=True)
    lift.ROOT = dest
    lift.OUT = dest/'generated/spu_lifted'
    lift.REG = dest/'spu_workloads.c'
    lift.TOOLS = base/('candidate' if a.lazy else 'baseline')/'tools'
    lift.REGCACHE = False
    original = lift.run
    excluded = {}
    for key in (('spu_regions',) if a.full_locals else ('spu_regpromo','spu_regions')):
        for image, blocks in target.TARGET.get(key,[]):
            excluded.setdefault(image,set()).update(blocks)

    def run(script,*args):
        if a.lazy and Path(script).name == 'spu_lifter.py':
            prefix = str(args[args.index('--symbol-prefix')+1])
            image = prefix.removeprefix('gt5p_')
            args = (*args,'--regcache-lazy','--regcache-exclude',
                    ','.join(hex(int(b,16)) for b in sorted(excluded.get(image,()))))
            if a.cache_ls: args=(*args,'--regcache-ls')
        return original(script,*args)

    lift.run = run
    sys.argv = [sys.argv[0]]
    lift.main()
    for module in (spu_native_patch,spu_regpromo,spu_region):
        if a.full_locals and module is spu_regpromo: continue
        module.GEN = lift.OUT
        result = module.main()
        if result:
            raise SystemExit(f'{module.__name__} failed: {result}')
    print(f'Private lift complete: {dest}',flush=True)


if __name__ == '__main__':
    main()
