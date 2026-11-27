"""Compile/run production cellSaveData.c against a synthetic game callback.

GT5 2.11 splits a save over 2 MiB into GT5.0, GT5.1, ... Loading: the file
list must be in the game's setBuf->buf, where its part loop looks for GT5.1
(#94/#95/#99; PS3_SAVEDATA_LIST_IN_BUF=0 old). Saving: reCreateMode YES must
remove files the save did not write, only after it succeeded
(PS3_SAVEDATA_RECREATE=0 old). No game, GPU or private inputs. --build-only
compiles without running the test.
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
folder = Path(tempfile.mkdtemp(prefix='savedata-split-', dir=root / 'scratch'))
batch = folder / 'build.cmd'
batch.write_text('@echo off\n'
    + 'set "PATH=C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer;%PATH%"\n'
    + 'call "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\n'
    + 'if errorlevel 1 exit /b 1\n'
    + '"C:\\Program Files\\LLVM\\bin\\clang-cl.exe" /nologo /O2 /MD /D_CRT_SECURE_NO_WARNINGS '
    + f'/I"{runtime}/libs/system" /I"{runtime}/include" /I"{runtime}" '
    + f'/Fo"{folder}/" /Fe"{folder}/test.exe" "{root}/scripts/test/savedata_split.c" /link /OPT:REF\n')
subprocess.run(['cmd', '/c', str(batch)], check=True)
print(f'Built {folder / "test.exe"}')
if not args.build_only:
    subprocess.run([str(folder / 'test.exe'), str(folder / 'savedata')], check=True, timeout=60)
