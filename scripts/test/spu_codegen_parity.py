"""Compare real lifted straight-line blocks and synthetic CFG/call cases.

Raw random states are not valid whole jobs: external transfers stop at the
boundary, internal loops are tested separately on controlled states. Runtime
calls are deterministic doubles that inspect and change registers/LS and log
channel traffic. This is an offline compiler/parity check, not game acceptance.
"""
import argparse
import ctypes
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'scripts'))
import spu_regpromo as rp
import spu_region
import spu_sysv
import target


def functions(path):
    lines = path.read_text().splitlines()
    lines = spu_region.strip(rp.strip([l for l in lines if '/* spu_native_patch */' not in l]))
    result = {}
    for i,line in enumerate(lines):
        match = re.fullmatch(r'void \w+_spu_func_([0-9A-F]{8})\(spu_context\* ctx\) \{',line)
        if match:
            end = lines.index('}',i+1)
            result[match[1]] = lines[i+1:end]
    return result


def stopped(body):
    out = []
    for line in body:
        line = rp.stopify(line)
        line = re.sub(r'\bgt5p_\w+_spu_func_([0-9A-F]{8})\(ctx\)',
                      lambda m:'fake_call(ctx,0x'+m[1]+'u)',line)
        out.append(line)
    return out


HARNESS = r'''
#include "spu_helpers.h"
#include <assert.h>
#include <stdio.h>
volatile int g_spu_fast=1, g_spu_chain_back=1;
unsigned g_spu_stack_reset_n;
int g_spu_ls_watch_n, g_spu_drain_ret_fast=1;
SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*);
static uint64_t traffic;
static void note(spu_context* c,unsigned op) {
    traffic=traffic*1099511628211ULL+op;
    for(unsigned i=0;i<128;i++) traffic=(traffic^c->gpr[i]._u32[0])*1099511628211ULL;
}
void spu_indirect_branch(spu_context* c) { note(c,1); }
void spu_stop(spu_context* c) { note(c,2); }
void spu_halt(spu_context* c) { note(c,3); }
void spu_drain_call_impl(spu_context* c,uint32_t pc) { note(c,pc); c->gpr[17]._u32[0]^=pc; g_spu_trampoline_fn=0; }
static void fake_call(spu_context* c,unsigned op) { note(c,op); c->gpr[3]=spu_xori(c->gpr[3],7); c->gpr[17]=spu_il(op); c->ls[op&0x3ffff]^=3; if(op==13)c->ls=c->ls_store+16; }
u128 spu_rdch(spu_context* c,uint32_t ch) { note(c,ch+100); c->gpr[7]=spu_il(ch); return spu_il(ch+3); }
uint32_t spu_rchcnt(spu_context* c,uint32_t ch) { note(c,ch+200); c->gpr[8]=spu_il(ch); return ch+1; }
void spu_wrch(spu_context* c,uint32_t ch,u128 v) { note(c,ch+300); c->gpr[9]=v; c->ls[ch&0x3ffff]=(uint8_t)v._u32[0]; }
'''
DRIVER = r'''
static spu_context a,b,init;
static uint32_t seed=0x20261114;
static uint32_t rnd(void) { seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed; }
static int words(const void* x,const void* y,size_t n) {
    const uint32_t *p=x,*q=y;
    for(size_t i=0;i<n/4;i++) if(p[i]!=q[i]) {
        /* Count NaN payload changes separately; no silent approximation. */
        if((p[i]&0x7f800000)==0x7f800000 && (q[i]&0x7f800000)==0x7f800000 && (p[i]&0x7fffff) && (q[i]&0x7fffff)) { nan_diffs++; continue; }
        return 0;
    }
    return 1;
}
int main(void) {
    unsigned cases=0;
    for(size_t p=0;p<sizeof pairs/sizeof pairs[0];p++) for(unsigned t=0;t<16;t++) {
        memset(&init,0,sizeof init); init.ls=init.ls_store;
        for(unsigned i=0;i<128;i++) for(unsigned j=0;j<4;j++) init.gpr[i]._u32[j]=rnd()&~0x40000000u;
        for(unsigned i=0;i<SPU_LS_SIZE;i+=4) { uint32_t w=rnd()&~0x40000000u; memcpy(init.ls+i,&w,4); }
        init.gpr[1]._u32[0]=4; init.host_depth=1; init.pc=0x4000; init.image_id=7; init.int_enable=t&1;
        if(!strcmp(pairs[p].name,"direct-call-tail-chain")) init.gpr[1]._u32[0]=100000;
        a=init; a.ls=a.ls_store; b=init;b.ls=b.ls_store;
        traffic=0;g_spu_trampoline_fn=0;pairs[p].a(&a);uint64_t ta=traffic;
        traffic=0;g_spu_trampoline_fn=0;pairs[p].b(&b);
        if(ta!=traffic || !words(a.gpr,b.gpr,sizeof a.gpr) || !words(a.ls_store,b.ls_store,SPU_LS_SIZE) || (a.ls-a.ls_store)!=(b.ls-b.ls_store) ||
           a.pc!=b.pc || a.int_enable!=b.int_enable || a.host_depth!=b.host_depth || a.image_id!=b.image_id || a.status!=b.status) {
            fprintf(stderr,"FAIL %s state %u pc %x/%x traffic %llx/%llx\n",pairs[p].name,t,(unsigned)a.pc,(unsigned)b.pc,ta,traffic);return 1;
        }
        cases++;
    }
    printf("parity: %zu blocks, %u states agree; NaN payload/sign differences=%u\n",sizeof pairs/sizeof pairs[0],cases,nan_diffs);
}
'''


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--image',default='j9804ABA8')
    ap.add_argument('--sysv',action='store_true')
    ap.add_argument('--fma',action='store_true')
    ap.add_argument('--new-root',default='lazy2')
    ap.add_argument('--cache-ls',action='store_true')
    args=ap.parse_args()
    ctypes.windll.kernel32.SetPriorityClass(ctypes.windll.kernel32.GetCurrentProcess(),0x40)
    base=ROOT/'scratch/spu-codegen'
    sys.path.insert(0,str(base/'candidate/tools'))
    from spu_regcache_lazy import localize
    old=functions(base/'reference/generated/spu_lifted'/args.image/'spu_recomp.c')
    new=functions(base/args.new_root/'generated/spu_lifted'/args.image/'spu_recomp.c')
    bodies=[];pairs=[]
    for key,body in old.items():
        if key not in new or any('goto loc_' in l or 'spu_unsupported(' in l for l in body):
            continue
        for suffix,lines in (('a',body),('b',new[key])):
            bodies += [f'void t_{key}_{suffix}(spu_context* ctx) {{']+stopped(lines)+['}']
        pairs.append(f'{{t_{key}_a,t_{key}_b,"{args.image}:{key}"}},')
    # Controlled CFG loops, forward joins and a context-mutating call in a loop.
    synthetic=[
        ['ctx->gpr[2] = spu_il(0);','loc_00000004:',
         'ctx->gpr[2] = spu_ai(ctx->gpr[2],1);','ctx->gpr[1] = spu_ai(ctx->gpr[1],-1);',
         'if (ctx->gpr[1]._u32[0]) goto loc_00000004;','/*@X*/return;'],
        ['if (ctx->gpr[5]._u32[0]&1) goto loc_00000008;',
         'ctx->gpr[2] = spu_il(3);','goto loc_0000000C;','loc_00000008:',
         'ctx->gpr[2] = spu_il(7);','loc_0000000C:',
         'ctx->gpr[3] = spu_a(ctx->gpr[2],ctx->gpr[3]);','/*@X*/return;'],
        ['loc_00000004:','ctx->gpr[2] = spu_a(ctx->gpr[2],ctx->gpr[3]);',
         '/*@O*/fake_call(ctx,7);/*@I*/','ctx->gpr[1] = spu_ai(ctx->gpr[1],-1);',
         'if (ctx->gpr[1]._u32[0]) goto loc_00000004;','/*@X*/return;'],
        ['ctx->gpr[2] = spu_ls_read128(ctx,0);','/*@O*/fake_call(ctx,13);/*@I*/',
         'ctx->gpr[3] = spu_ls_read128(ctx,16);','/*@X*/return;'],
    ]
    for i,body in enumerate(synthetic):
        bodies += [f'void synth{i}_a(spu_context* ctx) {{']+[re.sub(r'/\*@[XOI]\*/','',l) for l in body]+['}']
        bodies += [f'void synth{i}_b(spu_context* ctx) {{']+localize(body,cache_ls=args.cache_ls)+['}']
        pairs.append(f'{{synth{i}_a,synth{i}_b,"synthetic{i}"}},')
    # Real cross-function calls/tail calls: pointer entry stays Windows ABI,
    # body-to-body transfers use the converted ABI. Exercise a long tail chain
    # so an accidentally nested call cannot masquerade as bounded execution.
    bodies += [
        'void chain_leaf(spu_context* ctx) { ctx->gpr[3] = spu_ai(ctx->gpr[3],1); }',
        'void chain_tail(spu_context* ctx) {',
        'if (ctx->gpr[1]._u32[0]) { ctx->gpr[1] = spu_ai(ctx->gpr[1],-1); __attribute__((musttail)) return chain_tail(ctx); }',
        'chain_leaf(ctx);', '}',
        'void chain_expected(spu_context* ctx) { ctx->gpr[1]=spu_ai(ctx->gpr[1],-100000); chain_leaf(ctx); }',
    ]
    pairs.append('{chain_expected,chain_tail,"direct-call-tail-chain"},')
    text=HARNESS+'\n'+'\n'.join(bodies)
    if args.sysv:
        # transform() inserts after the conventional generated helper include.
        text=text.replace('#include "spu_helpers.h"','#include "../../runtime/spu/spu_helpers.h"')
        text=spu_sysv.transform(text)
    text=text.replace('"../../runtime/spu/spu_helpers.h"','"spu_helpers.h"')
    text+='\nstatic unsigned nan_diffs;\nstatic const struct { void (*a)(spu_context*);void (*b)(spu_context*); const char* name; } pairs[]={\n'+'\n'.join(pairs)+'\n};\n'+DRIVER
    out=Path(tempfile.mkdtemp(prefix=f'parity-{args.image}-',dir=base))
    (out/'test.c').write_text(text)
    vc=r'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat'
    flags='-mfma' if args.fma else ''
    if args.image in target.TARGET.get('audio_extended_zero_images',[]):
        flags+=f' /FI"{ROOT}/src/gt5p_spu_audio_math.h"'
    (out/'build.cmd').write_text('@echo off\nset "PATH=C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer;%PATH%"\n'+f'call "{vc}" >nul\n'+
       f'"C:/Program Files/LLVM/bin/clang-cl.exe" /nologo /O2 /MD /D_CRT_SECURE_NO_WARNINGS /DSPU_LS_NO_PROBES -msse4.1 {flags} /I"{base}/candidate/runtime/spu" /Fo"{out}/test.obj" /Fe"{out}/test.exe" "{out}/test.c"\n')
    subprocess.run(['cmd','/c',str(out/'build.cmd')],check=True)
    result=subprocess.run([str(out/'test.exe')],capture_output=True,text=True,timeout=180)
    (out/'stderr.log').write_text(result.stderr)
    print(result.stdout,end='')
    result.check_returncode()
    print('Evidence:',out)


if __name__=='__main__':
    main()
