"""Exercise the actual backend slot lookup/selection loop with fake resources.

No GPU/game/shader work. Own the shared validation mutex around this command.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import uuid


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    source = (args.runtime/'libs/video/rsx_d3d12_backend.c').read_text()
    flag = source[source.index('static int tex_lru_on(void)'):source.index('/* Slot last found')]
    begin = source.index('    const u32 key_off = off;')
    end = source.index('    /* RSX_TEXUPSTAT=1:', begin)
    block = source[begin:end]
    # Compile the production selector, including empty preference, live-slot
    # protection, exact identity, checksum and replacement-generation handling.
    work = root/'scratch'/('texture-slot-policy-'+uuid.uuid4().hex)
    work.mkdir(parents=True)
    (work/'selector.inc').write_text(flag+'\n'+'''
static int select_slot(u32 off, u32 w, u32 h, u32 fmt, int cube, u32 depth, u32 levels) {
u32 chain_bytes=4;
''' + block + '\nreturn slot >= 0 ? slot : freeslot;\n}\n')
    (work/'test.c').write_text(r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint32_t u32; typedef uint64_t u64; typedef uint8_t u8;
typedef struct { void* res; u32 key,w,h,fmt,depth,levels,off,csum; int cube,used;
    u64 last_used_frame; int repl_cand,repl_gen; } VPTexSlot;
static struct { VPTexSlot vp_tex[4]; u64 frame_count; } s_d3d;
static short s_tex_hint[4096];
#define TEX_HINT_N 4096u
static int s_tex_repl_gen;
static u8 bytes[16]; u8* vm_base=bytes;
static int vp_tex_cap(void) { return 4; }
static const u8* tex_src(u32 off,u32 nb) { (void)off;(void)nb;return bytes; }
static u32 tex_csum_timed(const u8* src,u32 nb) { (void)src;(void)nb;return 123; }
static void tex_vol_mark(u32 off,u32 nb) { (void)off;(void)nb; }
#include "selector.inc"
static void reset(void) {
    memset(&s_d3d,0,sizeof s_d3d); memset(s_tex_hint,0,sizeof s_tex_hint);
    s_d3d.frame_count=100;
    for(int i=0;i<4;i++) { VPTexSlot* c=&s_d3d.vp_tex[i];c->res=(void*)1;
        c->key=i+1;c->w=c->h=1;c->fmt=0x85;c->levels=1;c->csum=123;
        c->last_used_frame=(u64[]){90,80,10,70}[i]; }
}
static int find(u32 key) { return select_slot(key,1,1,0x85,0,0,1); }
int main(void) {
    int lru=tex_lru_on();
    reset(); assert(find(9)==(lru?2:0));
    /* A current-frame reference is protected even if its timestamp is old. */
    reset(); s_d3d.vp_tex[2].used=1; assert(find(9)==(lru?3:0));
    reset(); for(int i=0;i<4;i++)s_d3d.vp_tex[i].used=1; assert(find(9)==-1);
    /* A real empty slot wins over the oldest allocated resource. */
    reset(); s_d3d.vp_tex[1].res=NULL; assert(find(9)==1);
    /* A validated hit updates recency, then cannot be replaced this frame. */
    reset(); assert(find(3)==2); assert(s_d3d.vp_tex[2].used);
    assert(s_d3d.vp_tex[2].last_used_frame==100); assert(find(9)==(lru?3:0));
    /* Dirty contents and replacement generation reuse the exact entry. */
    reset(); s_d3d.vp_tex[1].csum=0; assert(find(2)==1);
    reset(); s_d3d.vp_tex[1].repl_cand=1;s_tex_repl_gen=1;assert(find(2)==1);
    /* Shape mismatch never returns an incompatible resource as a hit. */
    reset(); s_d3d.vp_tex[1].w=2;assert(find(2)==(lru?2:0));
    /* Equal-age candidates retain stable first-slot ordering. */
    reset(); for(int i=0;i<4;i++)s_d3d.vp_tex[i].last_used_frame=4;
    assert(find(9)==0);
    printf("texture slot policy: 10 cases passed; lru=%d\n",lru);
    return 0;
}
''')
    command = work/'build.cmd'
    command.write_text(f'''@echo off
set "PATH=C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer;%PATH%"
call "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul
"C:\\Program Files\\LLVM\\bin\\clang-cl.exe" /nologo /TC /O2 /MD /D_CRT_SECURE_NO_WARNINGS /I"{work}" /Fo"{work}/test.obj" /Fe"{work}/test.exe" "{work}/test.c"
exit /b %errorlevel%
''')
    subprocess.run(['cmd', '/c', str(command)], cwd=root, check=True)
    results = []
    for mode in ('0', '1'):
        result = subprocess.run([str(work/'test.exe')], env=dict(os.environ, RSX_TEX_LRU=mode),
                                capture_output=True, text=True, check=True)
        print(result.stdout, end='')
        results.append(dict(mode=mode, stdout=result.stdout))
    (work/'report.json').write_text(json.dumps(results, indent=2))
    print('Evidence:', work)


if __name__ == '__main__':
    main()
