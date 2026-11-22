"""Compare the native fill with the private lifted loop, including all GPRs/LS.

The extracted game-derived include stays in scratch; no game code is tracked.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
runtime = Path(os.environ.get("PS3RECOMP_DIR", root.parent / "ps3recomp-tourline5")).resolve()
folder = Path(tempfile.mkdtemp(prefix="spu-fill-", dir=root / "scratch"))
source = (root / "generated/spu_lifted/b01697100/spu_recomp.c").read_text()
head = "void gt5p_b01697100_spu_func_00005BA8(spu_context* ctx) {"
start = source.index(head)
end = source.index("        { ctx->pc = 0x5C50;", start)
loop = re.sub(r"^.*spu_native_patch.*\n", "", source[start:end], flags=re.M)
(folder / "fill.inc").write_text(loop.replace(head, "static void lifted_fill(spu_context* ctx) {") + "}\n")
batch = folder / "build.cmd"
batch.write_text('@echo off\nset "PATH=C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer;%PATH%"\n'
    'call "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\n'
    '"C:\\Program Files\\LLVM\\bin\\clang-cl.exe" /nologo /O2 /MD /D_CRT_SECURE_NO_WARNINGS /DSPU_LS_NO_PROBES -msse4.1 '
    f'/I"{runtime}/runtime/spu" /I"{folder}" /Fo"{folder}/" /Fe"{folder}/test.exe" "{root}/scripts/test/spu_fill.c"\n')
subprocess.run(["cmd", "/c", str(batch)], check=True)
subprocess.run([str(folder / "test.exe")], check=True, timeout=60)
