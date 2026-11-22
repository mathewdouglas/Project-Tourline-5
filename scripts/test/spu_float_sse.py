"""SSE forms of the SPU single-precision helpers (runtime spu_helpers.h: fa fs fm fceq fcgt
fma fms fnms) against the per-lane loops (-DSPU_FLOAT_SCALAR): every result bit, NaN
payloads included, on random and edge values, with MXCSR FTZ/DAZ off and on, built with and
without FMA. No game data."""
from pathlib import Path
import os, subprocess, sys, tempfile
root = Path(__file__).resolve().parents[2]
runtime = Path(os.environ.get('PS3RECOMP_DIR', root.parent / 'ps3recomp-tourline5')).resolve()
src = root / 'scripts' / 'test' / 'spu_float_sse'
folder = Path(tempfile.mkdtemp(prefix='spu-float-sse-', dir=root / 'scratch'))
clang = r'C:\Program Files\LLVM\bin\clang.exe'
fail = 0
for flags in (['-msse4.1'], ['-msse4.1', '-mfma']):
    exe = folder / ('t%d.exe' % len(flags))
    inc = ['-I', str(runtime / 'runtime' / 'spu')]
    subprocess.run([clang, '-O2', '-w', '-DSPU_LS_NO_PROBES', *flags, *inc, '-c', str(src / 'ops.c'), '-o', str(folder / 's.o')], check=True)
    subprocess.run([clang, '-O2', '-w', '-DSPU_LS_NO_PROBES', '-DVARIANT_V', *flags, *inc, '-c', str(src / 'ops.c'), '-o', str(folder / 'v.o')], check=True)
    subprocess.run([clang, '-O2', '-w', *flags, *inc, str(src / 'main.c'), str(folder / 's.o'), str(folder / 'v.o'), '-o', str(exe)], check=True)
    print('==', ' '.join(flags))
    fail |= subprocess.run([str(exe)]).returncode
sys.exit(fail)
