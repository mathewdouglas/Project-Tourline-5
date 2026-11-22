"""WARP test for DXC fallback VS + VS_PENDING PSO fix (#70/#71)."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
runtime = Path(os.environ.get('PS3RECOMP_DIR', root.parent / 'ps3recomp-tourline5'))
backend = (runtime / 'libs/video/rsx_d3d12_backend.c').read_text(encoding='utf-8')

assert 'RSX_VP_FALLBACK_DXC' in backend, 'missing RSX_VP_FALLBACK_DXC rollback switch'
assert 'RSX_VP_PENDING_NO_PSO' in backend, 'missing RSX_VP_PENDING_NO_PSO rollback switch'

work = Path(tempfile.mkdtemp(prefix='vp-fallback-pso-', dir=root / 'scratch'))
key = backend[backend.index('static u64 shader_source_key('):backend.index('static int dxbc_mem_on(')]
block = backend[backend.index('/* GT5_DXC_BEGIN:'):backend.index('/* GT5_DXC_END */')]
(work / 'compiler.inc').write_text(key + block, encoding='utf-8')
for name in ('dxcompiler.dll', 'dxil.dll'):
    shutil.copy2(root / 'third_party/dxc' / name, work / name)

(work / 'build.cmd').write_text(f'''@echo off
call "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
"C:\\Program Files\\LLVM\\bin\\clang-cl.exe" /nologo /TC /O2 /MD /D_CRT_SECURE_NO_WARNINGS /I"{work}" /Fo"{work}/test.obj" /Fe"{work}/test.exe" "{root}/scripts/test/vp_fallback_pso.c" /link d3d12.lib dxgi.lib d3dcompiler.lib dxguid.lib
''', encoding='ascii')
subprocess.run(['cmd', '/c', str(work / 'build.cmd')], check=True)

cache = work / 'cache'
cache.mkdir()
env = dict(os.environ, RSX_SHADER_CACHE=str(cache), RSX_SHADER_COMPILER='dxc')
subprocess.run([str(work / 'test.exe')], env=env, check=True, timeout=60)
print('Evidence:', work)
