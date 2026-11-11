"""Test the actual GT5 runtime compiler block using pinned DLLs and WARP."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
runtime = Path(os.environ.get('PS3RECOMP_DIR', root.parent / 'ps3recomp-tourline5'))
work = Path(tempfile.mkdtemp(prefix='shader-compiler-', dir=root / 'scratch'))
s = (runtime / 'libs/video/rsx_d3d12_backend.c').read_text(encoding='utf-8')
key = s[s.index('static u64 shader_source_key('):s.index('static int dxbc_mem_on(')]
block = s[s.index('/* GT5_DXC_BEGIN:'):s.index('/* GT5_DXC_END */')]
(work / 'compiler.inc').write_text(key + block, encoding='utf-8')
for name in ('dxcompiler.dll', 'dxil.dll'):
    shutil.copy2(root / 'third_party/dxc' / name, work / name)
(work / 'build.cmd').write_text(f'''@echo off
set "PATH=C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer;%PATH%"
call "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
"C:\\Program Files\\LLVM\\bin\\clang-cl.exe" /nologo /TC /O2 /MD /D_CRT_SECURE_NO_WARNINGS /I"{work}" /Fo"{work}/test.obj" /Fe"{work}/test.exe" "{root}/scripts/test/shader_compiler.c" /link d3d12.lib dxgi.lib d3dcompiler.lib dxguid.lib
''', encoding='ascii')
subprocess.run(['cmd', '/c', str(work / 'build.cmd')], check=True)
missing = work / 'missing'; missing.mkdir(); shutil.copy2(work / 'test.exe', missing / 'test.exe')
missing_validator = work / 'missing-validator'; missing_validator.mkdir()
for name in ('test.exe', 'dxcompiler.dll'): shutil.copy2(work / name, missing_validator / name)
for mode, expected, exe in [('dxc', 1, work), ('fxc', 0, work), ('invalid', 0, work), ('dxc', 0, missing), ('dxc', 0, missing_validator), ('unset', 1, work), ('unset', 0, missing)]:
    cache = work / ('cache-' + mode + '-' + exe.name); cache.mkdir()
    sentinel = cache / 'fxc-sentinel'; sentinel.write_bytes(b'preserve')
    env = dict(os.environ, RSX_SHADER_CACHE=str(cache), RSX_SHADER_COMPILER=mode)
    if mode == 'unset': env.pop('RSX_SHADER_COMPILER')
    env.pop('RSX_SHADER_SRC_DUMP', None)
    subprocess.run([str(exe / 'test.exe'), str(expected), str(cache)], env=env, check=True, timeout=60)
    assert sentinel.read_bytes() == b'preserve'
print('Evidence:', work)
