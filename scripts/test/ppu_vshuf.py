"""Exactness of the PPU AltiVec byte shuffles (runtime/ppu/ppu_vshuf.h) against
the scalar loops the lifter emitted before (PPU_VSHUF=0): vperm, lvlx, lvrx,
lvsl/lvsr. No game data."""
from pathlib import Path
import os, subprocess, tempfile
root = Path(__file__).resolve().parents[2]
runtime = Path(os.environ.get('PS3RECOMP_DIR', root.parent / 'ps3recomp-tourline5')).resolve()
folder = Path(tempfile.mkdtemp(prefix='ppu-vshuf-', dir=root / 'scratch'))
batch = folder / 'build.cmd'
batch.write_text('@echo off\nset "PATH=C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer;%PATH%"\n'
                 + 'call "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\n'
                 + '"C:\\Program Files\\LLVM\\bin\\clang-cl.exe" /nologo /O2 /MD -msse4.1 '
                 + f'/I"{runtime}/runtime/ppu" /Fo"{folder}/" /Fe"{folder}/test.exe" "{root}/scripts/test/ppu_vshuf.c"\n')
subprocess.run(['cmd', '/c', str(batch)], check=True)
subprocess.run([str(folder / 'test.exe')], check=True, timeout=300)
