"""Banded texture decode (RSX_TEX_DECODE_THREADS) against the whole-image decode, on synthetic data.
Builds scripts/test/tex_decode_bands.c with the runtime's rsx_texture_layout.c (PS3RECOMP_DIR,
default ../ps3recomp-tourline5) and runs it."""
from pathlib import Path
import os, subprocess, tempfile
root = Path(__file__).resolve().parents[2]
runtime = Path(os.environ.get('PS3RECOMP_DIR', root.parent / 'ps3recomp-tourline5')).resolve()
folder = Path(tempfile.mkdtemp(prefix='tex-bands-', dir=root / 'scratch'))
exe = folder / 'test.exe'
subprocess.run([r'C:\Program Files\LLVM\bin\clang.exe', '-O2', '-Wno-deprecated-declarations',
                '-I', str(runtime / 'libs/video'), '-I', str(runtime / 'include'),
                str(root / 'scripts/test/tex_decode_bands.c'), str(runtime / 'libs/video/rsx_texture_layout.c'),
                '-o', str(exe)], check=True)
subprocess.run([str(exe)], check=True, timeout=120)
