"""Exercise production exact-alias binding on all texture units, without game data."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
runtime = Path(os.environ.get('PS3RECOMP_DIR', root.parent / 'ps3recomp-tourline5'))
source = (runtime / 'libs/video/rsx_d3d12_backend.c').read_text(encoding='utf-8')
folder = Path(tempfile.mkdtemp(prefix='rt-self-read-', dir=root / 'scratch'))
start = source.index('                    /* Exact aliases also reach this path')
end = source.index('                    continue;', start)
binding = source[start:end]
start = source.index('                /* A self-read may have resolved this target')
end = source.index('                if (cur_rt < 0) {', start)
dirty = source[start:end]
field = re.search(r'\bu\d+\s+tex_self;', source)[0]
harness = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned u32;
typedef int ID3D12Resource;
typedef int DXGI_FORMAT;
#define D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING 0
typedef struct { u32 stencil, rt_off, rt_mrt[3]; int tex_rt[16]; u32 self_tgt; FIELD } Draw;
static ID3D12Resource original, snapshot;
static struct { struct { ID3D12Resource *res, *ms; int dxgi, ms_dirty; } off_rt[4]; } s_d3d;
static ID3D12Resource *bound;
static int allocation_fails;
static int off_rt_find(u32 off) { return off ? (int)off - 1 : -1; }
static ID3D12Resource *off_rt_self_ensure(int rt) { (void)rt; return allocation_fails ? NULL : &snapshot; }
static void srv_write(u32 slot, ID3D12Resource *res, DXGI_FORMAT fmt, unsigned map) {
    (void)slot; (void)fmt; (void)map; bound=res;
}
static void bind(Draw *dr, int rt, int _u) {
    u32 wslot=0;
BINDING
}
static void written(int cur_rt, int cur_m[3]) {
DIRTY
}
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); return 1; } } while(0)
int main(void) {
    int rollback=getenv("RT_NO_SELFCOPY") != NULL;
    for (int i=0;i<4;i++) { s_d3d.off_rt[i].res=&original; s_d3d.off_rt[i].ms=&original; }
    for (int unit=0;unit<16;unit++) {
        Draw d={0}; d.rt_off=1;
        bind(&d,0,unit);
        CHECK(bound==(rollback ? &original : &snapshot));
        CHECK(d.tex_self==(rollback ? 0 : (1u<<unit)));
        CHECK(d.tex_rt[unit]==(rollback ? 0 : -1));
        memset(&d,0,sizeof d); d.rt_off=2; d.rt_mrt[2]=1;
        bind(&d,0,unit); CHECK(bound==(rollback ? &original : &snapshot));
        memset(&d,0,sizeof d); d.rt_off=2;
        bind(&d,0,unit); CHECK(bound==&original && d.tex_self==0 && d.tex_rt[unit]==0);
        d.rt_off=1; d.stencil=1u<<26;
        bind(&d,0,unit); CHECK(bound==&original && d.tex_self==0);
        d.stencil=0; allocation_fails=1;
        bind(&d,0,unit); CHECK(bound==&original && d.tex_rt[unit]==0);
        allocation_fails=0;
    }
    int mrt[3]={1,-1,3}; s_d3d.off_rt[3].ms=NULL;
    written(0,mrt);
    CHECK(s_d3d.off_rt[0].ms_dirty && s_d3d.off_rt[1].ms_dirty);
    CHECK(!s_d3d.off_rt[2].ms_dirty && !s_d3d.off_rt[3].ms_dirty);
    s_d3d.off_rt[0].ms_dirty=0; written(0,mrt);
    CHECK(s_d3d.off_rt[0].ms_dirty); /* another draw after a resolve */
    puts("PASS all 16 units, primary/MRT aliases, mask exclusion, allocation failure, dirty after resolve");
    return 0;
}
'''.replace('FIELD', field).replace('BINDING', binding).replace('DIRTY', dirty)
(folder / 'test.c').write_text(harness, encoding='utf-8')
batch = folder / 'build.cmd'
batch.write_text('@echo off\nset "PATH=C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer;%PATH%"\n'
    'call "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\n'
    'if errorlevel 1 exit /b 1\n'
    '"C:\\Program Files\\LLVM\\bin\\clang-cl.exe" /nologo /O2 /MD /std:c17 /D_CRT_SECURE_NO_WARNINGS '
    f'/Fo"{folder}/test.obj" /Fe"{folder}/test.exe" "{folder}/test.c"\n')
subprocess.run(['cmd', '/c', str(batch)], check=True)
for rollback in [False, True]:
    env = os.environ.copy()
    env.pop('RT_NO_SELFCOPY', None)
    if rollback:
        env['RT_NO_SELFCOPY'] = '1'
    subprocess.run([str(folder / 'test.exe')], env=env, check=True)
