"""Compile production pressure, register-sizing and sample-sharing helpers.

No game data or GPU required. Includes rollback modes and nonliteral register
indices, which must retain the conservative register file.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--runtime', type=Path, default=Path(os.environ.get(
    'PS3RECOMP_DIR', ROOT.parent / 'ps3recomp-tourline5')))
args = parser.parse_args()
runtime = args.runtime.resolve()
work = Path(tempfile.mkdtemp(prefix='prologue-ports-', dir=ROOT/'scratch'))


def block(source, start, end):
    first = source.index(start)
    return source[first:source.index(end, first)]


pad = (runtime/'libs/input/cellPad.c').read_text(encoding='utf-8')
fp = (runtime/'libs/video/rsx_fp_decompiler.c').read_text(encoding='utf-8')
backend = (runtime/'libs/video/rsx_d3d12_backend.c').read_text(encoding='utf-8')
pressure = block(pad, '        static const struct { u8 word; u8 bit; u8 off; } pm[]', '\n    }\n')
registers = block(fp, '#define FP_REG_HEAD', 'static int rsx_fp_decompile_internal(')
sample = block(backend, 'static int fp_sample_share_on(void)', 'static int texture_volume_on(void)')
fixture = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libs/input/cellPad.h"
typedef struct { char* p; u32 cap; u32 len; int ok; } Out;
''' + registers + sample + '\nstatic void fill(CellPadData* data) {\n' + pressure + r'''
}
static void sized(const char* body, const char* expected) {
    char text[4096]; snprintf(text,sizeof text,"%s%s",FP_REG_HEAD,body);
    Out o={text,sizeof text,(u32)strlen(text),1};
    fp_size_registers(&o);
    assert(strstr(text,expected)); assert(o.len==strlen(text));
    assert(!strcmp(text+o.len-strlen(body),body));
}
int main(void) {
    const char* e=getenv("PAD_PRESS_FORCE_FULL"); int legacy=e && e[0]=='1';
    const char* r=getenv("RSX_FP_REG_SIZE"); int shrink=!(r && !strcmp(r,"0"));
    const char* s=getenv("RSX_FP_SAMPLE_SHARE"); int share=!(s && !strcmp(s,"0"));
    CellPadData d;
    for (int v=0;v<=255;v++) for(int digital=0;digital<2;digital++) {
        memset(&d,0,sizeof d);
        d.button[CELL_PAD_BTN_OFFSET_DIGITAL2]=digital?3:0;
        d.button[CELL_PAD_BTN_OFFSET_PRESS_L2]=v;
        d.button[CELL_PAD_BTN_OFFSET_PRESS_R2]=255-v;
        fill(&d);
        assert(d.button[CELL_PAD_BTN_OFFSET_PRESS_L2]==(digital && (legacy || !v)?255:v));
        assert(d.button[CELL_PAD_BTN_OFFSET_PRESS_R2]==(digital && (legacy || v==255)?255:255-v));
    }
    memset(&d,0,sizeof d);d.button[CELL_PAD_BTN_OFFSET_DIGITAL2]=0x40;fill(&d);
    assert(d.button[CELL_PAD_BTN_OFFSET_PRESS_CROSS]==255);
    sized("    r[0] = h[0];\n",shrink?"float4 r[1]; float4 h[1];":"float4 r[48]; float4 h[48];");
    sized("    r[47] = h[12];\n",shrink?"float4 r[48]; float4 h[13];":"float4 r[48]; float4 h[48];");
    sized("    r[index] = h[0];\n","float4 r[48]; float4 h[48];");
    sized("    r[48] = h[0];\n","float4 r[48]; float4 h[48];");
    unsigned masks[]={0,1u<<16,2u<<16,4u<<16,(1u<<30)|(1u<<16),(1u<<30)|(4u<<16)};
    for(unsigned i=0;i<sizeof masks/sizeof masks[0];i++)
        assert(fp_sample_qualifier(masks[i])==(share || ((masks[i]>>16)&15)>1));
    puts("PASS: 1024 trigger pressures, injected Cross, register bounds/fallback and sample qualifiers");
    return 0;
}
'''
(work/'test.c').write_text(fixture, encoding='utf-8')
(work/'build.cmd').write_text(f'''@echo off
set "PATH=C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer;%PATH%"
call "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
"C:\\Program Files\\LLVM\\bin\\clang-cl.exe" /nologo /O2 /MD /D_CRT_SECURE_NO_WARNINGS /I"{runtime}" /I"{runtime}/include" /Fo"{work}/test.obj" /Fe"{work}/test.exe" "{work}/test.c"
''', encoding='ascii')
subprocess.run(['cmd','/c',str(work/'build.cmd')],check=True)
for mode in ('default','rollback'):
    env=os.environ.copy()
    for key in ('PAD_PRESS_FORCE_FULL','RSX_FP_REG_SIZE','RSX_FP_SAMPLE_SHARE'):
        env.pop(key,None)
    if mode=='rollback':
        env.update(PAD_PRESS_FORCE_FULL='1',RSX_FP_REG_SIZE='0',RSX_FP_SAMPLE_SHARE='0')
    subprocess.run([str(work/'test.exe')],env=env,check=True,timeout=10)
print('Evidence:',work)
