"""Compile audio-only SPU arithmetic regressions in portable/scalar/FMA modes."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
runtime = Path(os.environ.get("PS3RECOMP_DIR", root.parent / "ps3recomp-tourline5"))
folder = Path(tempfile.mkdtemp(prefix="audio-extended-", dir=root / "scratch"))
vc = ('@echo off\nset "PATH=C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer;%PATH%"\n'
      'call "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\n')
for name, flags in (("portable", "-msse4.1"), ("scalar", "-msse4.1 /DSPU_FI_SCALAR /DSPU_FMA_LIBM"), ("native", "-msse4.1 -mfma")):
    command = (f'"C:/Program Files/LLVM/bin/clang-cl.exe" /nologo /O2 /MD /D_CRT_SECURE_NO_WARNINGS '
               f'/DSPU_LS_NO_PROBES {flags} /I"{runtime}/runtime/spu" /I"{root}/src" '
               f'/Fo"{folder}/{name}.obj" /Fe"{folder}/{name}.exe" "{root}/scripts/test/audio_extended.c"\n')
    batch = folder / f"{name}.cmd"
    batch.write_text(vc + command)
    subprocess.run(["cmd", "/c", str(batch)], check=True)
    subprocess.run([str(folder / f"{name}.exe")], check=True, timeout=30)
