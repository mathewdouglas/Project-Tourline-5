"""GT5 Phase 42: the lock-line lock under priority inversion (no game needed).

Builds scripts/test/lockline_inversion.c against the runtime's real
runtime/spu/spu_coherency.c (PS3RECOMP_DIR, else ../ps3recomp-tourline5) and
runs it with the waiters' sleep off (SPU_LOCKLINE_SLEEP_US=0, the old spin),
at its default, and explicitly on. With the sleep, every waiter must get the
lock within 100 ms of the IDLE-priority holder starting its 2 ms critical
section. The old spin is reported, not judged: how long Windows takes to
boost a starved thread varies, though it is usually seconds.
"""
from pathlib import Path
import os, re, subprocess, tempfile

root = Path(__file__).resolve().parents[2]
runtime = Path(os.environ.get('PS3RECOMP_DIR', root.parent / 'ps3recomp-tourline5')).resolve()
src = runtime / 'runtime/spu/spu_coherency.c'
assert 'SPU_LOCKLINE_SLEEP_US' in src.read_text(encoding='utf-8'), f'{src} lacks the Phase 42 change'
folder = Path(tempfile.mkdtemp(prefix='lockline-', dir=root / 'scratch'))
batch = folder / 'build.cmd'
batch.write_text('@echo off\nset "PATH=C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer;%PATH%"\n'
    + 'call "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\n'
    + '"C:\\Program Files\\LLVM\\bin\\clang-cl.exe" /nologo /O2 /MD /D_CRT_SECURE_NO_WARNINGS '
    + f'/I"{runtime}/include" /I"{runtime}/runtime/spu" /I"{runtime}/runtime/ppu" /I"{runtime}/runtime/memory" '
    + f'/Fo"{folder}/" /Fe"{folder}/test.exe" "{root}/scripts/test/lockline_inversion.c" "{src}" '
    # The file's other functions reference runtime state the test never
    # reaches (timeline, SPU contexts); only the lock functions are called.
    + '/link /FORCE:UNRESOLVED >nul\n')
subprocess.run(['cmd', '/c', str(batch)], check=True)


def run(value):
    env = os.environ.copy()
    env.pop('SPU_LOCKLINE_SLEEP_US', None)
    env['SPU_LOCKLINE_WARN_MS'] = '0'
    if value is not None:
        env['SPU_LOCKLINE_SLEEP_US'] = value
    out = subprocess.run([str(folder / 'test.exe')], env=env, check=True, timeout=60,
                         capture_output=True, text=True).stdout.strip()
    ms = float(re.search(r'after ([\d.]+) ms', out).group(1))
    return out, ms


out, ms = run('0')
print(f'old spin (SPU_LOCKLINE_SLEEP_US=0): {out}')
for label, value in (('default', None), ('SPU_LOCKLINE_SLEEP_US=1000', '1000')):
    out, ms = run(value)
    print(f'{label}: {out}')
    assert 'waiters done 3/3' in out and ms < 100.0, f'{label}: holder starved ({ms:.1f} ms)'
print('lock-line waiters let an IDLE-priority holder finish')
