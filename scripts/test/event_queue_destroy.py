"""Compile/run production event-queue teardown against synthetic Windows workers.

No game, GPU or private inputs. --build-only compiles without running the test.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build-only', action='store_true')
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
runtime = Path(os.environ.get('PS3RECOMP_DIR', root.parent / 'ps3recomp-tourline5')).resolve()
folder = Path(tempfile.mkdtemp(prefix='event-queue-', dir=root / 'scratch'))
batch = folder / 'build.cmd'
batch.write_text('@echo off\n'
    + 'set "PATH=C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer;%PATH%"\n'
    + 'call "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\n'
    + 'if errorlevel 1 exit /b 1\n'
    + '"C:\\Program Files\\LLVM\\bin\\clang-cl.exe" /nologo /O2 /MD /D_CRT_SECURE_NO_WARNINGS /c '
    + f'/Fo"{folder}/timer.obj" "{runtime}/runtime/syscalls/sys_timer.c"\n'
    + 'if errorlevel 1 exit /b 1\n'
    + '"C:\\Program Files\\LLVM\\bin\\clang-cl.exe" /nologo /O2 /MD /Gy /D_CRT_SECURE_NO_WARNINGS '
    + f'/I"{runtime}/runtime/syscalls" /Fo"{folder}/" /Fe"{folder}/test.exe" '
    + f'"{root}/scripts/test/event_queue_destroy.c" /link /OPT:REF\n')
subprocess.run(['cmd', '/c', str(batch)], check=True)
print(f'Built {folder / "test.exe"}')
if not args.build_only:
    # A broken destroy must fail promptly, rather than hanging a test runner.
    subprocess.run([str(folder / 'test.exe')], check=True, timeout=30)
