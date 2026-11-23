"""Bit-exact tests for gt5p_cull_f970 (b0196FF40 0xF970) and gt5p_rain_a220 (b016B5B00 0xA220)."""
from pathlib import Path
import os, shutil, subprocess, sys, tempfile

root = Path(__file__).resolve().parents[2]
runtime = Path(os.environ.get("PS3RECOMP_DIR", root.parent / "ps3recomp-tourline5")).resolve()
folder = Path(tempfile.mkdtemp(prefix="spu-extra-", dir=root / "scratch"))

try:
    # 1. Extract lifted b0196FF40 0xF970_oc
    s_cull = (root / "generated/spu_lifted/b0196FF40/spu_recomp.c").read_text(encoding="utf-8")
    h1 = "static void gt5p_b0196FF40_spu_func_0000F970_oc(spu_context* ctx) {"
    i1 = s_cull.index(h1)
    j1 = s_cull.index("\nstatic void gt5p_b0196FF40_spu_func_0000F970_rpc(", i1)
    (folder / "f970_oc.inc").write_text(s_cull[i1:j1] + "\n", encoding="utf-8")

    # 2. Extract lifted b016B5B00 0xA128..0xA2AC
    s_rain = (root / "generated/spu_lifted/b016B5B00/spu_recomp.c").read_text(encoding="utf-8")
    h2 = "void gt5p_b016B5B00_spu_func_0000A128(spu_context* ctx) {"
    i2 = s_rain.index(h2)
    j2 = s_rain.index("\nvoid gt5p_b016B5B00_spu_func_0000A2C8(", i2)
    # Strip spu_native_patch lines so lifted reference runs unmodified
    rain_inc = "\n".join(l for l in s_rain[i2:j2].splitlines() if "/* spu_native_patch */" not in l)
    (folder / "rain_a220.inc").write_text(rain_inc + "\n", encoding="utf-8")

    c_src = folder / "test_extra.c"
    c_src.write_text(r'''
#include "spu_context.h"
#include "spu_helpers.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

volatile int g_spu_chain_back = 1;
unsigned g_spu_stack_reset_n = 0;
SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*) = NULL;
volatile int g_spu_fast = 0;
int g_spu_drain_ret_fast = 0;
void spu_drain_call_impl(spu_context* ctx, uint32_t ret_pc) { (void)ctx; (void)ret_pc; }
void spu_indirect_branch(spu_context* ctx) { (void)ctx; }

static void gt5p_b016B5B00_spu_func_0000A128(spu_context* ctx);
static void gt5p_b016B5B00_spu_func_0000A180(spu_context* ctx);
static void gt5p_b016B5B00_spu_func_0000A214(spu_context* ctx);
static void gt5p_b016B5B00_spu_func_0000A220(spu_context* ctx);
static void gt5p_b016B5B00_spu_func_0000A270(spu_context* ctx);
static void gt5p_b016B5B00_spu_func_0000A290(spu_context* ctx);
static void gt5p_b016B5B00_spu_func_0000A2A0(spu_context* ctx);
static void gt5p_b016B5B00_spu_func_0000A2AC(spu_context* ctx);

#include "f970_oc.inc"
#include "rain_a220.inc"
#include "gt5p_native_cull.c"
#include "gt5p_native_eiger.c"

static uint32_t xrnd(uint32_t* s) { uint32_t x = *s; x ^= x<<13; x ^= x>>17; x ^= x<<5; *s = x; return x; }
static float rf(uint32_t* s, float scale) { return ((int32_t)(xrnd(s) % 2001) - 1000) * (scale / 1000.0f); }
static void st_be32(uint8_t* p, uint32_t v) { v = __builtin_bswap32(v); memcpy(p, &v, 4); }
static void st_be_f32(uint8_t* p, float f) { uint32_t v; memcpy(&v, &f, 4); st_be32(p, v); }
static u128 splat_f(float f) { u128 r; for (int i = 0; i < 4; i++) r._f32[i] = f; return r; }
static u128 mk_u(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { u128 r; r._u32[0]=a; r._u32[1]=b; r._u32[2]=c; r._u32[3]=d; return r; }

static int test_f970(int fused) {
    static spu_context a, b;
    static uint8_t lsa[SPU_LS_SIZE], lsb[SPU_LS_SIZE];
    uint32_t seed = 0xCAFEBABEu;
    const int ep_regs[] = {2,6,8,9,12,13,16,21,22,23,24,25,27,28};
    for (int tc = 0; tc < 200; tc++) {
        memset(&a, 0, sizeof a); memset(lsa, 0, sizeof lsa); a.ls = lsa;
        const uint32_t nv4 = (uint32_t)(4 * (1 + (tc % 16)));
        const uint32_t bytes = nv4 * 16u;
        const uint32_t src_base = 0x10000u, dst_base = 0x20000u, r7_addr = 0x30000u;
        for (uint32_t i = 0; i < nv4; i++) {
            st_be_f32(lsa + src_base + 16*i + 0, rf(&seed, 50.0f));
            st_be_f32(lsa + src_base + 16*i + 4, rf(&seed, 50.0f));
            st_be_f32(lsa + src_base + 16*i + 8, rf(&seed, 50.0f));
            st_be_f32(lsa + src_base + 16*i + 12, 1.0f);
        }
        for (int k = 0; k < 4; k++) st_be32(lsa + r7_addr + 4*k, xrnd(&seed));
        /* Constants from 0xF6C0..0xF970 */
        a.gpr[10] = mk_u(0x04050607u, 0x14151617u, 0x00010203u, 0x10111213u);
        a.gpr[14] = mk_u(0x0C0D0E0Fu, 0x1C1D1E1Fu, 0x08090A0Bu, 0x18191A1Bu);
        a.gpr[15] = mk_u(0x00010203u, 0x10111213u, 0x04050607u, 0x14151617u);
        a.gpr[20] = mk_u(0x08090A0Bu, 0x18191A1Bu, 0x0C0D0E0Fu, 0x1C1D1E1Fu);
        a.gpr[30] = mk_u(0x18191A1Bu, 0x1C1D1E1Fu, 0x00010203u, 0x04050607u);
        a.gpr[36] = mk_u(0x00010203u, 0x04050607u, 0x18191A1Bu, 0x1C1D1E1Fu);
        a.gpr[40] = mk_u(0xC0C00405u, 0x06070809u, 0x0A0B1213u, 0x80808080u);
        a.gpr[46] = spu_fsmbi(0xFF);
        a.gpr[16] = spu_il(0);
        a.gpr[13] = spu_ilhu(0x3F80);
        a.gpr[11] = spu_ilhu(0x3400);
        a.gpr[25] = spu_splat_u32(0x7FFFFFFFu);
        a.gpr[45] = spu_splat_u32((tc & 1) ? 0x80000000u : 0u);
        a.gpr[7]  = spu_splat_u32(r7_addr);
        a.gpr[9]  = spu_splat_u32(xrnd(&seed));
        a.gpr[12] = spu_splat_u32(xrnd(&seed) & 3u);
        a.gpr[24] = spu_splat_u32(xrnd(&seed) & 3u);
        a.gpr[26] = spu_splat_u32(nv4);
        /* Matrix rows */
        const int mregs[] = {34,28,37,32, 22,21,6,38, 39,35,29,23, 2,42,27,33};
        for (int k = 0; k < 16; k++) a.gpr[mregs[k]] = splat_f(rf(&seed, 4.0f));
        a.gpr[32] = splat_f((tc & 2) ? -5.0f : 5.0f); /* test both positive & negative W */
        const uint32_t neg = (uint32_t)(-(int32_t)bytes);
        a.gpr[31] = spu_splat_u32(neg);
        a.gpr[8]  = spu_splat_u32(src_base + bytes);
        a.gpr[53] = spu_splat_u32(src_base + bytes + 16u);
        a.gpr[54] = spu_splat_u32(src_base + bytes + 32u);
        a.gpr[55] = spu_splat_u32(src_base + bytes + 48u);
        a.gpr[41] = spu_splat_u32(dst_base + bytes);
        a.gpr[43] = spu_splat_u32(dst_base + bytes + 16u);
        a.gpr[49] = spu_splat_u32(dst_base + bytes + 32u);
        a.gpr[51] = spu_splat_u32(dst_base + bytes + 48u);
        u128 sh116d0 = mk_u(0xC0C01213u, 0x12131213u, 0x12131213u, 0x12131213u);
        a.gpr[47] = spu_shufb(a.gpr[31], a.gpr[31], sh116d0);

        b = a; memcpy(lsb, lsa, sizeof lsa); b.ls = lsb;
        gt5p_b0196FF40_spu_func_0000F970_oc(&a);
        if (!gt5p_cull_f970(&b, fused)) { fprintf(stderr, "FAIL f970 bailed tc=%d\n", tc); return 1; }
        if (memcmp(lsa + dst_base, lsb + dst_base, bytes) != 0) {
            fprintf(stderr, "FAIL f970 LS mismatch tc=%d nv4=%u\n", tc, nv4);
            for (uint32_t i = 0; i < bytes; i += 4) {
                uint32_t va, vb; memcpy(&va, lsa+dst_base+i, 4); memcpy(&vb, lsb+dst_base+i, 4);
                if (va != vb) { fprintf(stderr, "  off +%u: lifted=%08X native=%08X\n", i, __builtin_bswap32(va), __builtin_bswap32(vb)); break; }
            }
            return 1;
        }
        for (size_t k = 0; k < sizeof(ep_regs)/sizeof(*ep_regs); k++) {
            int r = ep_regs[k];
            if (memcmp(&a.gpr[r], &b.gpr[r], 16) != 0) { fprintf(stderr, "FAIL f970 reg r%d mismatch tc=%d\n", r, tc); return 1; }
        }
    }
    printf("PASS: 200 randomized b0196FF40 0xF970 states bit-exact (fused=%d)\n", fused);
    return 0;
}

static int test_rain_a220(int fused) {
    static spu_context a, b;
    static uint8_t lsa[SPU_LS_SIZE], lsb[SPU_LS_SIZE];
    uint32_t seed = 0x13572468u;
    for (int tc = 0; tc < 500; tc++) {
        memset(&a, 0, sizeof a); memset(lsa, 0, sizeof lsa); a.ls = lsa;
        const uint32_t hdr = 0x10000u, pt = 0x20000u;
        const int n = tc % 9;
        st_be32(lsa + hdr, (uint32_t)(n & 0xFFFF)); /* low half of word 0 = _s16[0] */
        st_be_f32(lsa + pt + 0, rf(&seed, 10.0f));
        st_be_f32(lsa + pt + 4, rf(&seed, 10.0f));
        st_be_f32(lsa + pt + 8, rf(&seed, 10.0f));
        st_be_f32(lsa + pt + 12, 1.0f);
        for (int k = 0; k < n; k++) {
            uint32_t vol = hdr + 528u + (uint32_t)(k * 208);
            /* Center near pt half the time so plane loop runs */
            st_be_f32(lsa + vol + 0xC0 + 0, rf(&seed, 5.0f));
            st_be_f32(lsa + vol + 0xC0 + 4, rf(&seed, 5.0f));
            st_be_f32(lsa + vol + 0xC0 + 8, rf(&seed, 5.0f));
            st_be_f32(lsa + vol + 0xC0 + 12, (tc & 1) ? 500.0f : 1.0f);
            for (int p = 0; p < 12; p++) {
                st_be_f32(lsa + vol + p*16 + 0, rf(&seed, 1.0f));
                st_be_f32(lsa + vol + p*16 + 4, rf(&seed, 1.0f));
                st_be_f32(lsa + vol + p*16 + 8, rf(&seed, 1.0f));
                st_be_f32(lsa + vol + p*16 + 12, (tc % 3 == 0) ? -20.0f : rf(&seed, 2.0f));
            }
        }
        a.gpr[1] = spu_splat_u32(0x38000u);
        a.gpr[3] = spu_splat_u32(hdr);
        a.gpr[4] = spu_splat_u32(pt);
        b = a; memcpy(lsb, lsa, sizeof lsa); b.ls = lsb;
        gt5p_b016B5B00_spu_func_0000A220(&a);
        if (!gt5p_rain_a220(&b, fused)) { fprintf(stderr, "FAIL rain_a220 bailed tc=%d\n", tc); return 1; }
        if (a.gpr[3]._u32[0] != b.gpr[3]._u32[0]) {
            fprintf(stderr, "FAIL rain_a220 mismatch tc=%d lifted=%u native=%u\n", tc, a.gpr[3]._u32[0], b.gpr[3]._u32[0]);
            return 1;
        }
    }
    printf("PASS: 500 randomized b016B5B00 0xA220 states bit-exact (fused=%d)\n", fused);
    return 0;
}

int main(void) {
    int fused = 0;
#if defined(__FMA__)
    fused = 1;
#endif
    return test_f970(fused) | test_rain_a220(fused);
}
''', encoding="utf-8")

    for name, extra in [("sse41", "-msse4.1"), ("fma", "-msse4.1 -mfma"), ("avx2_fma", "-mavx2 -mfma")]:
        exe = folder / f"test_{name}.exe"
        bat = folder / f"build_{name}.cmd"
        bat.write_text(
            '@echo off\n'
            'set "PATH=%PATH%;C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer"\n'
            'call "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\n'
            f'"C:\\Program Files\\LLVM\\bin\\clang-cl.exe" /nologo /O2 /MD /D_CRT_SECURE_NO_WARNINGS '
            f'/DSPU_LS_NO_PROBES /DSPU_CODEGEN_HELPERS {extra} '
            f'/I"{runtime}/runtime/spu" /I"{root}/src" /I"{folder}" '
            f'/Fo"{folder}/" /Fe"{exe}" "{c_src}"\n',
            encoding="utf-8",
        )
        subprocess.run(["cmd", "/c", str(bat)], check=True)
        subprocess.run([str(exe)], check=True, timeout=60)
finally:
    shutil.rmtree(folder, ignore_errors=True)
