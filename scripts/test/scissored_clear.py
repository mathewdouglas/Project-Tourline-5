"""Exercise production clear recording and replay rectangle arithmetic, without a game."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
runtime = Path(os.environ.get('PS3RECOMP_DIR', root.parent / 'ps3recomp-tourline5'))
folder = Path(tempfile.mkdtemp(prefix='scissored-clear-', dir=root / 'scratch'))
source = (runtime / 'libs/video/rsx_d3d12_backend.c').read_text()
start = source.index('static int display_rect_clear(')
end = source.index('static void d3d12_set_render_target(', start)
recording = source[start:end]
start = source.index('                    D3D12_RECT cr =')
end = source.index('                    /* RT_CLEARDBG=1:', start)
rectangle = source[start:end]
harness = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rsx_commands.h"
typedef unsigned long long u64;
typedef long LONG;
typedef unsigned UINT;
typedef struct { LONG left, top, right, bottom; } D3D12_RECT;
typedef struct {
    int is_vp, is_clear, tex_slot, zonly;
    u32 rt_off, rt_w, rt_h, rt_fmt, rt_mrt[3], zeta_off;
    u32 sc_x, sc_y, sc_w, sc_h;
    u8 clear_z_flags, clear_z_stencil;
    float cc[4], clear_z;
} D3D12DrawRecord;
#define MAX_DRAWS 32
static u32 s_draw_cap = MAX_DRAWS, s_gw = 1280, s_gh = 720;
static struct { u32 draw_count; float clear_color[4]; D3D12DrawRecord draws[MAX_DRAWS]; } frame;
#define FR (&frame)
static struct {
    const rsx_state* current_rsx_state;
    unsigned long long frame_count;
    u32 vb_offset, vp_vb_offset, width, height;
    int initialized;
} s_d3d;
static u32 s_mask_zeta_off, s_clear_presents, s_dbg_clears_since_present;
static unsigned s_clear_present_flip, flips = 1;
static int presents, resets;
static u32 target;
static int blink_dbg(void) { return 0; }
static int rsx_stencil_mask_pass(const rsx_state* st) { (void)st; return 0; }
static int depth_keep_on(void) { return 1; }
static int zb_on(void) { return 1; }
static u32 depth_clear_flags(const rsx_state* st, u32 flags) {
    return st && st->surface_zeta_offset ? flags & 3u : 0;
}
static u32 current_rt_off(u32* w, u32* h, u32 mrt[3]) {
    *w = 1280; *h = 720; memset(mrt, 0, 3*sizeof(u32)); return target;
}
static void dr_note_surface(D3D12DrawRecord* dr) { (void)dr; }
static void dr_set_scissor(D3D12DrawRecord* dr, const rsx_state* st) {
    dr->sc_x=st->scissor_x; dr->sc_y=st->scissor_y;
    dr->sc_w=st->scissor_w; dr->sc_h=st->scissor_h;
}
unsigned cellGcm_flip_request_count(void) { return flips; }
static void render_frame(void) { presents++; }
static void ix_frame_reset(void) { resets++; }
'''
harness += recording
harness += '\nstatic int replay_rect(D3D12DrawRecord* dr, D3D12_RECT* out) {\nint cur_rt=-1;\nfor(int once=1;once;once=0) {\n'
harness += rectangle
harness += '\n*out=cr; return nr;\n}\nreturn -1;\n}\n'
harness += r'''
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); return 1; } } while(0)
static void seed(void) {
    memset(&frame,0,sizeof frame); frame.draw_count=6;
    frame.clear_color[0]=0.75f;
    presents=resets=0; target=0; flips++;
}
int main(void) {
    rsx_state st={0}; st.color_target=1; st.surface_zeta_offset=0x1100000;
    st.surface_clip_w=2048; st.surface_clip_h=1080;
    s_d3d.current_rsx_state=&st; s_d3d.initialized=1;
    const char* mode=getenv("RSX_SCISSORED_CLEAR");
    const int fixed=!(mode && mode[0]=='0');
    st.scissor_w=1280; st.scissor_h=720;
    seed(); d3d12_clear(NULL,0xf1,0xff102030,1.0f,0);
    CHECK(presents==1 && resets==1 && frame.draw_count==0);
    st.scissor_x=385; st.scissor_y=135; st.scissor_w=506; st.scissor_h=480;
    seed(); d3d12_clear(NULL,0xf1,0xff102030,0.25f,0);
    if (fixed) {
        CHECK(presents==0 && resets==0 && frame.draw_count==8);
        CHECK(frame.draws[6].clear_z_flags==1 && frame.draws[6].clear_z==0.25f);
        CHECK(frame.draws[7].is_clear && !frame.draws[7].clear_z_flags);
        CHECK(frame.draws[7].rt_off==0 && frame.draws[7].sc_x==385);
        CHECK(frame.draws[7].sc_y==135 && frame.draws[7].sc_w==506 && frame.draws[7].sc_h==480);
        CHECK(frame.clear_color[0]==0.75f); /* panel clear must not set next whole-frame clear */
        CHECK(frame.draws[7].cc[0]==16.0f/255 && frame.draws[7].cc[3]==1.0f);
    } else CHECK(presents==1 && resets==1 && frame.draw_count==0);
    seed(); target=0x2880000; d3d12_clear(NULL,0xf1,0,1,0);
    CHECK(presents==0 && frame.draw_count==8 && frame.draws[7].rt_off==target);
    seed(); d3d12_clear(NULL,1,0,0.5f,0);
    CHECK(presents==0 && frame.draw_count==7 && frame.draws[6].clear_z_flags==1);
    seed(); d3d12_clear(NULL,0,0,1,0);
    CHECK(presents==0 && frame.draw_count==6);
    seed(); frame.draw_count=MAX_DRAWS;
    if(fixed) { d3d12_clear(NULL,0xf1,0,1,0); CHECK(frame.draw_count==MAX_DRAWS && presents==0); }
    D3D12DrawRecord dr={0}; D3D12_RECT r;
    dr.sc_x=385; dr.sc_y=135; dr.sc_w=506; dr.sc_h=480;
    s_d3d.width=1280; s_d3d.height=720;
    CHECK(replay_rect(&dr,&r)==1 && r.left==385 && r.top==135 && r.right==891 && r.bottom==615);
    s_d3d.width=1707;
    CHECK(replay_rect(&dr,&r)==1 && r.left==513 && r.right==1188 && r.bottom==615);
    s_d3d.width=3840; s_d3d.height=2160;
    CHECK(replay_rect(&dr,&r)==1 && r.left==1155 && r.top==405 && r.right==2673 && r.bottom==1845);
    dr.sc_x=1279; dr.sc_y=719; dr.sc_w=100; dr.sc_h=100;
    CHECK(replay_rect(&dr,&r)==1 && r.right==3840 && r.bottom==2160);
    dr.sc_x=1300; CHECK(replay_rect(&dr,&r)==-1);
    memset(&dr,0,sizeof dr); CHECK(replay_rect(&dr,&r)==0);
    puts(fixed ? "PASS scissored clears: ordered colour/depth, no mid-frame present, scaled/clipped rectangles" :
                 "PASS rollback reproduces mid-frame presentation; rectangle replay checks pass");
    return 0;
}
'''
(folder / 'test.c').write_text(harness)
batch = folder / 'build.cmd'
batch.write_text('@echo off\nset "PATH=C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer;%PATH%"\n'
    'call "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\n'
    'if errorlevel 1 exit /b 1\n'
    '"C:\\Program Files\\LLVM\\bin\\clang-cl.exe" /nologo /O2 /MD /std:c17 /D_CRT_SECURE_NO_WARNINGS '
    f'/I"{runtime}/include" /I"{runtime}/libs/video" /Fo"{folder}/test.obj" /Fe"{folder}/test.exe" "{folder}/test.c"\n')
subprocess.run(['cmd','/c',str(batch)],check=True)
for mode in [None, '0']:
    env=os.environ.copy()
    for key in ['RSX_SCISSORED_CLEAR','RSX_ACCUM_FRAME','CLEARDBG','RSX_CLEAR_TRACE','RSX_CLEAR_PRESENT_STICKY','CLEAR_RGB']:
        env.pop(key,None)
    if mode is not None: env['RSX_SCISSORED_CLEAR']=mode
    subprocess.run([str(folder/'test.exe')],env=env,check=True)
