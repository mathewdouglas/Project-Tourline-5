"""Exercise the real chaining transform on synthetic nested SPU calls.

No game inputs: isolate the transform section rather than importing the lifter
(its disassembler loads a private ELF at import time). The tiny dispatcher
models spu_drain_call_impl's pending-return check, including a return that is
also a lifted entry. Compile with musttail enabled as in the player build.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "scripts/lift_spu_jobs.py").read_text(encoding="utf-8")
scope = dict(os=os, re=re)
exec(source[source.index("FUNC_RE ="):source.index("def main():")], scope)
folder = Path(tempfile.mkdtemp(prefix="spu-chain-", dir=root / "scratch"))

fixture = r'''#include "fixture.h"
void test_spu_func_00000100(spu_context* ctx) {
    { int32_t _si = ctx->image_id; ctx->host_depth++; test_spu_func_00000300(ctx); spu_drain_call(ctx, 0x180); ctx->host_depth--; spu_img_restore(ctx, _si); }
    assert(ctx->drain_ret_pc == 0x200);
    ctx->trace = ctx->trace * 10 + 3;
    ctx->pc = ctx->gpr[2]; g_spu_trampoline_fn = spu_indirect_branch; return;
}
void test_spu_func_00000180(spu_context* ctx) {
    exit(77); /* Must return to the suspended host caller instead. */
}
void test_spu_func_00000200(spu_context* ctx) {
    exit(77);
}
void test_spu_func_00000300(spu_context* ctx) {
    ctx->trace = ctx->trace * 10 + 1;
    ctx->pc = ctx->gpr[3]; g_spu_trampoline_fn = spu_indirect_branch; return;
}
void test_spu_func_00000320(spu_context* ctx) {
    ctx->trace = ctx->trace * 10 + 2;
    ctx->pc = 0x180; g_spu_trampoline_fn = spu_indirect_branch; return;
}
void test_spu_func_00000400(spu_context* ctx) {
    { int32_t _si = ctx->image_id; ctx->host_depth++; test_spu_func_00000100(ctx); spu_drain_call(ctx, 0x200); ctx->host_depth--; spu_img_restore(ctx, _si); }
    ctx->trace = ctx->trace * 10 + 4;
}
void test_spu_func_00000500(spu_context* ctx) {
    { int32_t _si = ctx->image_id; ctx->pc = 0x100; ctx->host_depth++; spu_indirect_branch(ctx); spu_drain_call(ctx, 0x200); ctx->host_depth--; spu_img_restore(ctx, _si); }
    ctx->trace = ctx->trace * 10 + 4;
}
void test_spu_func_00000600(spu_context* ctx) {
    ctx->pc = 0x0; g_spu_trampoline_fn = spu_indirect_branch; return;
}
void test_spu_func_00000700(spu_context* ctx) {
    { int32_t _si = ctx->image_id; ctx->host_depth++; test_spu_func_00000600(ctx); spu_drain_call(ctx, 0x0); ctx->host_depth--; spu_img_restore(ctx, _si); }
}
'''
header = r'''#include <stdint.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#define SPU_LS_MASK 0x3FFFFu
typedef struct spu_context {
    uint32_t pc, drain_ret_pc, host_depth, image_id, int_enable, event_status, event_mask, trace, gpr[4];
} spu_context;
volatile int g_gt5p_chain_ib, g_spu_fast = 1, g_yz_lockstep_on, g_spu_chain_back = 1;
unsigned g_spu_stack_reset_n;
void (*g_spu_trampoline_fn)(spu_context*);
void spu_indirect_branch(spu_context*);
void spu_drain_call(spu_context*, uint32_t);
void spu_img_restore(spu_context* ctx, int32_t image) { ctx->image_id = image; }
void test_spu_func_00000100(spu_context*);
void test_spu_func_00000180(spu_context*);
void test_spu_func_00000200(spu_context*);
void test_spu_func_00000300(spu_context*);
void test_spu_func_00000320(spu_context*);
void test_spu_func_00000600(spu_context*);
'''
driver = r'''
static unsigned dispatches;
void spu_indirect_branch(spu_context* ctx) {
    switch (ctx->pc) {
    case 0x100: test_spu_func_00000100(ctx); break;
    case 0x180: test_spu_func_00000180(ctx); break;
    case 0x200: test_spu_func_00000200(ctx); break;
    case 0x320: test_spu_func_00000320(ctx); break;
    default: abort();
    }
}
void spu_drain_call(spu_context* ctx, uint32_t ret) {
    while (g_spu_trampoline_fn) {
        if (g_spu_trampoline_fn == spu_indirect_branch && ctx->pc == ret) {
            g_spu_trampoline_fn = 0;
            break;
        }
        void (*fn)(spu_context*) = g_spu_trampoline_fn;
        g_spu_trampoline_fn = 0;
        ctx->drain_ret_pc = ret;
        dispatches++;
        fn(ctx);
    }
    assert(ctx->pc == ret);
    ctx->drain_ret_pc = 0;
}
int main(void) {
    for (unsigned mode = 0; mode < 7; mode++) {
        g_gt5p_chain_ib = mode != 0;
        g_spu_fast = mode != 2;
        g_yz_lockstep_on = mode == 3;
        g_spu_stack_reset_n = mode == 4;
        g_spu_chain_back = mode != 6;
        for (unsigned indirect = 0; indirect < 2; indirect++) {
            spu_context ctx = {0};
            ctx.gpr[2] = 0x200; ctx.gpr[3] = 0x320;
            ctx.int_enable = ctx.event_status = ctx.event_mask = mode == 5;
            dispatches = 0;
            if (indirect) test_spu_func_00000500(&ctx);
            else test_spu_func_00000400(&ctx);
            assert(ctx.trace == 1234 && ctx.host_depth == 0 && ctx.drain_ret_pc == 0);
            assert(dispatches == (mode == 1 ? 0 : 1));
        }
        spu_context ctx = {0};
        ctx.drain_ret_pc = 0xABC;
        test_spu_func_00000700(&ctx);
        assert(ctx.pc == 0 && ctx.drain_ret_pc == 0xABC && ctx.host_depth == 0);
        ctx = (spu_context){0};
        ctx.gpr[3] = 0x320;
        test_spu_func_00000300(&ctx);
        assert(ctx.trace == 1 && g_spu_trampoline_fn == spu_indirect_branch);
        spu_drain_call(&ctx, 0x180);
        assert(ctx.trace == 12 && ctx.drain_ret_pc == 0);
    }
    puts("nested direct/indirect calls, pending lifted returns, rollback and guards: PASS");
}
'''
(folder / "fixture.h").write_text(header)
lifted = folder / "spu_recomp.c"
lifted.write_text(fixture)
scope["CHAIN_IB"] = False
scope["chain_forward"](folder)
assert "_chain_ret" not in lifted.read_text() and "g_gt5p_chain_ib" not in lifted.read_text()
scope["CHAIN_IB"] = True
lifted.write_text(fixture)
scope["chain_forward"](folder)
first = lifted.read_text()
scope["chain_forward"](folder)
assert lifted.read_text() == first, "transform must be idempotent"
lifted.write_text(first + driver)
batch = folder / "build.cmd"
batch.write_text(
    '@echo off\nset "PATH=C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer;%PATH%"\n'
    'call "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\n'
    '"C:\\Program Files\\LLVM\\bin\\clang-cl.exe" /nologo /O2 /MD /std:c17 '
    f'/Fo"{folder}/" /Fe"{folder}/test.exe" "{lifted}"\n')
subprocess.run(["cmd", "/c", str(batch)], check=True)
subprocess.run([str(folder / "test.exe")], check=True, timeout=10)
# Recreate the missing pending-return check from the failed experiment. The
# same fixture must reject it by reaching the forbidden lifted return entry.
broken = first.replace("(ctx->drain_ret_pc && ((ctx->pc ^ ctx->drain_ret_pc) & SPU_LS_MASK) == 0)", "0")
assert broken != first
lifted.write_text(broken + driver)
subprocess.run(["cmd", "/c", str(batch)], check=True)
assert subprocess.run([str(folder / "test.exe")], timeout=10).returncode == 77
print("missing return boundary correctly rejected: PASS")
