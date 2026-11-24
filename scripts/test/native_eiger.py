"""Compare gt5p_eiger_8298 against lifted gt5p_b01697100_region_00008298 (bit-exact)."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
runtime = Path(os.environ.get("PS3RECOMP_DIR", root.parent / "ps3recomp-tourline5")).resolve()
folder = Path(tempfile.mkdtemp(prefix="eiger-8298-", dir=root / "scratch"))
source = (root / "generated/spu_lifted/b01697100/spu_recomp.c").read_text(encoding="utf-8")
head = "static void gt5p_b01697100_region_00008298(spu_context* ctx) {"
start = source.index(head)
end = source.index("/* spu_region end */", start)
(folder / "eiger_8298.inc").write_text(source[start:end], encoding="utf-8")

for name, extra in [("sse41", "-msse4.1"), ("fma", "-msse4.1 -mfma")]:
    exe = folder / f"test_{name}.exe"
    batch = folder / f"build_{name}.cmd"
    batch.write_text(
        '@echo off\n'
        'set "PATH=C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer;%PATH%"\n'
        'call "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\n'
        f'"C:\\Program Files\\LLVM\\bin\\clang-cl.exe" /nologo /O2 /MD /D_CRT_SECURE_NO_WARNINGS '
        f'/DSPU_LS_NO_PROBES /DSPU_CODEGEN_HELPERS {extra} '
        f'/I"{runtime}/runtime/spu" /I"{root}/src" /I"{folder}" '
        f'/Fo"{folder}/" /Fe"{exe}" "{root}/scripts/test/native_eiger.c"\n',
        encoding="utf-8",
    )
    subprocess.run(["cmd", "/c", str(batch)], check=True)
    subprocess.run([str(exe)], check=True, timeout=60)
