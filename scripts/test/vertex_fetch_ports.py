"""Render synthetic vertex programs through both fetch emitters on WARP.

Ports Prologue's bit-for-bit GPU comparison harness. The inputs below are
synthetic MOV programs, covering single attributes and all sixteen together.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--runtime',type=Path,default=Path(os.environ.get('PS3RECOMP_DIR',root.parent/'ps3recomp-tourline5')))
args=parser.parse_args()
runtime=args.runtime.resolve()
work=Path(tempfile.mkdtemp(prefix='vertex-fetch-ports-',dir=root/'scratch'))
(work/'emit.c').write_text(r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rsx_vp_decompiler.h"
int main(int argc,char** argv) {
    assert(argc==2);
    for(unsigned test=0;test<17;test++) {
        u32 code[16*4]={0};
        for(unsigned i=0;i<16;i++) {
            unsigned attr=test==16?i:test;
            unsigned src=2u|(0x1bu<<8);
            code[i*4]=(63u<<15)|(1u<<30);
            code[i*4+1]=(1u<<22)|(attr<<8)|(src>>9);
            code[i*4+2]=(src&511u)<<23;
            code[i*4+3]=(i==15?1u:0u)|(i<<2)|(15u<<13);
        }
        char out[262144];
        assert(rsx_vp_decompile_gpufetch((const u8*)code,sizeof code,out,sizeof out)==16);
        char path[1024];snprintf(path,sizeof path,"%s/test%02u.gpu1.vs_5_0.hlsl",argv[1],test);
        FILE* f=fopen(path,"wb");assert(f);fwrite(out,1,strlen(out),f);fclose(f);
    }
    return 0;
}
''',encoding='ascii')
(work/'build.cmd').write_text(f'''@echo off
set "PATH=C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer;%PATH%"
call "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
"C:\\Program Files\\LLVM\\bin\\clang-cl.exe" /nologo /O2 /MD /D_CRT_SECURE_NO_WARNINGS /I"{runtime}/include" /I"{runtime}/libs/video" /Fo"{work}/" /Fe"{work}/emit.exe" "{work}/emit.c" "{runtime}/libs/video/rsx_vp_decompiler.c"
if errorlevel 1 exit /b 1
"C:\\Program Files\\LLVM\\bin\\clang-cl.exe" /nologo /O2 /MD /EHsc /std:c++20 /D_CRT_SECURE_NO_WARNINGS /Fo"{work}/" /Fe"{work}/equiv.exe" "{root}/scripts/test/vertex_fetch_equiv.cpp" d3d11.lib d3dcompiler.lib
''',encoding='ascii')
subprocess.run(['cmd','/c',str(work/'build.cmd')],check=True)
for mode in ('0','1','2'):
    folder=work/mode;folder.mkdir()
    subprocess.run([str(work/'emit.exe'),str(folder)],env=dict(os.environ,RSX_VFETCH_LOOP=mode),check=True)
for mode in ('1','2'):
    subprocess.run([str(work/'equiv.exe'),str(work/'0'),str(work/mode),'--warp','--trials','16','--vertices','128'],check=True,timeout=120)
print('Evidence:',work)
