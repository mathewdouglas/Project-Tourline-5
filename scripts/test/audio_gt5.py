"""Compare GT5's actual hot mixer loops to scalar fused arithmetic; private code stays in scratch."""
from pathlib import Path
import os, re, subprocess, tempfile
root=Path(__file__).resolve().parents[2]
runtime=Path(os.environ.get('PS3RECOMP_DIR',root.parent/'ps3recomp-tourline5')).resolve()
folder=Path(tempfile.mkdtemp(prefix='audio-gt5-',dir=root/'scratch'))
s=(root/'generated/spu_lifted/jD58063CD/spu_recomp.c').read_text()
blocks=[]
for addr in ('00004720','00004C80'):
 m=re.search(r'void gt5p_jD58063CD_spu_func_'+addr+r'\(spu_context\* ctx\) \{.*?\n}',s,re.S)
 if not m: raise SystemExit('GT5 mixer loop missing: '+addr)
 block=m.group()
 block=re.sub(r'__attribute__\(\(musttail\)\) return gt5p_\w+\(ctx\);','return;',block)
 block=block.replace('gt5p_jD58063CD_spu_func_'+addr,'MIXER_'+addr)
 blocks.append(block)
(folder/'kernels.inc').write_text('\n'.join(blocks))
(folder/'kernels.c').write_text('#include "spu_helpers.h"\n#define CAT_(a,b) a##b\n#define CAT(a,b) CAT_(a,b)\n'
 '#define MIXER_00004720 CAT(NAME,_4720)\n#define MIXER_00004C80 CAT(NAME,_4C80)\n#include "kernels.inc"\n')
clang='"C:\\Program Files\\LLVM\\bin\\clang-cl.exe"'
flags=f'/nologo /O2 /MD /D_CRT_SECURE_NO_WARNINGS /DSPU_LS_NO_PROBES /I"{runtime}/runtime/spu"'
lines=['@echo off','set "PATH=C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer;%PATH%"',
 'call "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul']
for name,extra in [('reference','-msse4.1 /DSPU_FMA_LIBM'),('fast','-msse4.1 -mfma')]:
 lines += [f'{clang} {flags} {extra} /DNAME={name} /c /Fo"{folder}/{name}.obj" "{folder}/kernels.c"','if errorlevel 1 exit /b 1']
lines += [f'{clang} {flags} /Fo"{folder}/" /Fe"{folder}/test.exe" "{root}/scripts/test/audio_gt5.c" "{folder}/reference.obj" "{folder}/fast.obj"']
batch=folder/'build.cmd';batch.write_text('\n'.join(lines)+'\n')
subprocess.run(['cmd','/c',str(batch)],check=True)
subprocess.run([str(folder/'test.exe')],check=True,timeout=60)
