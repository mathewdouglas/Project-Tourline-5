"""Race pack and helper-thread prewarm: the real backend code on D3D12 WARP, then the Python pack tool.

Extracts the backend from its hashing helpers to the end of the pipeline
queue, compiles it with synthetic pipelines and a scratch shader cache, and
runs it. Afterwards the Python reader checks the pack the renderer wrote and
rebuilds it from the same files: the two must be byte-identical.
No game, game data or GPU is used.
"""
from pathlib import Path
import os
import shutil
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / 'scripts'))
import race_preload_pack as pack  # noqa: E402

runtime = Path(os.environ.get('PS3RECOMP_DIR', root.parent / 'ps3recomp-tourline5'))
work = root / 'scratch/race-prewarm-test'
cache = work / 'cache'
if cache.exists():
    shutil.rmtree(cache)
(cache / 'warmup-v1').mkdir(parents=True)
source = (runtime / 'libs/video/rsx_d3d12_backend.c').read_text(encoding='utf-8')
block = source[source.index('static u64 pso_hash_bytes('):source.index('/* PSO_ASYNC_END */')]
(work / 'backend.inc').write_text(block, encoding='utf-8')
cmd = work / 'build.cmd'
cmd.write_text(f'''@echo off
call "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul
"C:\\Program Files\\LLVM\\bin\\clang-cl.exe" /nologo /TC /O2 /MD /D_CRT_SECURE_NO_WARNINGS /I"{work}" /Fo"{work}/test.obj" /Fe"{work}/test.exe" "{root}/scripts/test/race_prewarm.c" /link d3d12.lib dxgi.lib d3dcompiler.lib uuid.lib
if errorlevel 1 exit /b 1
"{work}/test.exe" "{cache.as_posix()}"
''', encoding='ascii')
subprocess.run(['cmd', '/c', str(cmd)], cwd=root, check=True)

warm = cache / 'warmup-v1'
native = (warm / 'race-pack-v1.bin').read_bytes()
parsed = pack.parse_bytes(native)
assert parsed['count'] == 48 and parsed['code_count'] == 4, (parsed['count'], parsed['code_count'])
listed = pack.index_hashes(warm / 'race-v1.txt')
assert [record[0] for record in parsed['records']] == listed
rebuilt = pack.build_bytes(warm)
assert rebuilt == native, 'Python and renderer packs differ'
report = pack.check(warm)
assert report['matches_index'] and report['same_bytes_as_rebuild'], report
blobs = sum(1 for record in parsed['records'] if record[6])
print(f'PASS: Python reads the renderer pack ({len(native)} bytes, 48 recipes, 4 shared shaders, {blobs} blobs) and rebuilds it byte for byte')

# The reader refuses what the renderer refuses.
for label, damaged in (
        ('payload', native[:-50] + bytes([native[-50] ^ 1]) + native[-49:]),
        ('truncated', native[:len(native) // 2]),
        ('magic', b'\0' + native[1:]),
        ('repeated member', native[:40 + 32] + native[40:40 + 8] + native[40 + 32 + 8:])):
    try:
        pack.parse_bytes(damaged)
    except ValueError:
        continue
    raise AssertionError(f'{label}: damaged pack accepted')
# Repeated and junk index lines do not change the pack.
index = warm / 'race-v1.txt'
original = index.read_text(encoding='ascii')
index.write_text(original + original.splitlines()[3] + '\nnot-a-hash\n\n0000000000000000\n', encoding='ascii')
assert pack.build_bytes(warm) == native
index.write_text(original, encoding='ascii')
print('PASS: Python reader rejects damaged packs; repeated, empty and junk index lines are ignored')
