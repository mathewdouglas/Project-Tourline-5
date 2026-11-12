"""GPU/driver stamp and the before-start shader preparation (GT5 Phase 44) on D3D12 WARP.

Extracts the real backend from its hashing helpers to the start of
rsx_d3d12_backend_prewarm, compiles it with gpu_stamp.c and runs one phase per
process over a scratch shader cache: learn, same driver, changed driver, the
run after, the off switch and an empty cache. Between learn and the driver
change every file is dated an hour back, as files from before an update are.
No game, game data or GPU is used.
"""
from pathlib import Path
import os
import shutil
import subprocess
import time

root = Path(__file__).resolve().parents[2]
runtime = Path(os.environ.get('PS3RECOMP_DIR', root.parent / 'ps3recomp-tourline5'))
work = root / 'scratch/gpu-stamp-test'
cache = work / 'cache'
empty = work / 'empty'
for d in (cache, empty):
    if d.exists():
        shutil.rmtree(d)
    (d / 'warmup-v1').mkdir(parents=True)
source = (runtime / 'libs/video/rsx_d3d12_backend.c').read_text(encoding='utf-8')
block = source[source.index('static u64 pso_hash_bytes('):source.index('void rsx_d3d12_backend_prewarm(void) {')]
(work / 'backend.inc').write_text(block, encoding='utf-8')
cmd = work / 'build.cmd'
cmd.write_text(f'''@echo off
call "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul
"C:\\Program Files\\LLVM\\bin\\clang-cl.exe" /nologo /TC /O2 /MD /D_CRT_SECURE_NO_WARNINGS /I"{work}" /Fo"{work}/test.obj" /Fe"{work}/test.exe" "{root}/scripts/test/gpu_stamp.c" /link d3d12.lib dxgi.lib d3dcompiler.lib uuid.lib
if errorlevel 1 exit /b 1
''', encoding='ascii')
subprocess.run(['cmd', '/c', str(cmd)], cwd=root, check=True)
exe = work / 'test.exe'
clean = {k: v for k, v in os.environ.items()
         if not k.startswith(('RSX_', 'GT5P_'))}


def phase(mode, where=cache):
    subprocess.run([str(exe), str(where), mode], cwd=root, check=True, env=clean)


phase('learn')
phase('same')
old = time.time() - 3600
for f in cache.rglob('*'):
    if f.is_file() and f.name != 'gpu-stamp.txt':
        os.utime(f, (old, old))
phase('changed')
phase('after')
phase('off')
phase('empty', empty)
print('PASS: GPU/driver stamp and before-start preparation')
