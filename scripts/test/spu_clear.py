"""Compare the GT5 native clear with its actual private lifted loop and full LS/register state."""
from pathlib import Path
import os, re, subprocess, tempfile
root=Path(__file__).resolve().parents[2]
runtime=Path(os.environ.get('PS3RECOMP_DIR',root.parent/'ps3recomp-tourline5')).resolve()
folder=Path(tempfile.mkdtemp(prefix='spu-clear-',dir=root/'scratch'))
s=(root/'generated/spu_lifted/b0196FF40/spu_recomp.c').read_text()
a=s.index('void gt5p_b0196FF40_spu_func_00004C14(')
b=s.index('        ctx->gpr[17] = spu_il(148);',a)
loop=s[a:b].replace('gt5p_b0196FF40_spu_func_00004C14','lifted_clear')
loop=re.sub(r'^.*spu_native_patch.*\n','',loop,flags=re.M)+'}\n'
(folder/'clear.inc').write_text(loop)
batch=folder/'build.cmd'
batch.write_text('@echo off\nset "PATH=C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer;%PATH%"\n'
 + 'call "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\n'
 + '"C:\\Program Files\\LLVM\\bin\\clang-cl.exe" /nologo /O2 /MD /D_CRT_SECURE_NO_WARNINGS /DSPU_LS_NO_PROBES -msse4.1 '
 + f'/I"{runtime}/runtime/spu" /I"{folder}" /Fo"{folder}/" /Fe"{folder}/test.exe" "{root}/scripts/test/spu_clear.c"\n')
subprocess.run(['cmd','/c',str(batch)],check=True)
subprocess.run([str(folder/'test.exe')],check=True,timeout=60)
