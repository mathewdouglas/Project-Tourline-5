"""Compile and run the emulated Logitech G27 checks (scripts/test/wheel_g27_test.c).

Builds the runtime's libs/hardware/wheel_g27.c with a test harness: USB
descriptors, the input report GT5's wheel driver reads, and the force-feedback
command decoder. No game, wheel, GPU or private inputs.
--build-only compiles without running.
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
(root / 'scratch').mkdir(exist_ok=True)
folder = Path(tempfile.mkdtemp(prefix='wheel-g27-', dir=root / 'scratch'))
batch = folder / 'build.cmd'
batch.write_text('@echo off\n'
    + 'set "PATH=C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer;%PATH%"\n'
    + 'call "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\n'
    + 'if errorlevel 1 exit /b 1\n'
    + '"C:\\Program Files\\LLVM\\bin\\clang-cl.exe" /nologo /O2 /MD /D_CRT_SECURE_NO_WARNINGS '
    + f'/I"{runtime}/include" /I"{runtime}/libs/hardware" /Fo"{folder}/" /Fe"{folder}/test.exe" '
    + f'"{root}/scripts/test/wheel_g27_test.c" "{runtime}/libs/hardware/wheel_g27.c" /link /SUBSYSTEM:CONSOLE\n')
subprocess.run(['cmd', '/c', str(batch)], check=True)
print(f'Built {folder / "test.exe"}')
if not args.build_only:
    subprocess.run([str(folder / 'test.exe')], check=True, timeout=30)
