#!/usr/bin/env python3
"""Bit-exact check of SSE4.1 spu_frest, spu_frsqest, spu_csflt, spu_cuflt,
spu_cflts, spu_gb, and spu_fsm against -DSPU_FLOAT_SCALAR."""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RUNTIME = Path(os.environ.get("PS3RECOMP_DIR") or (ROOT.parent / "ps3recomp-tourline5"))
CLANG = Path(r"C:\Program Files\LLVM\bin\clang-cl.exe")

HARNESS = r'''
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#define SPU_LS_NO_PROBES
#define SPU_CODEGEN_HELPERS
#include "spu_context.h"
#include "spu_helpers.h"

#ifdef REF_SCALAR
#define FN(x) ref_##x
#else
#define FN(x) sse_##x
#endif

u128 FN(frest)(u128 a)   { return spu_frest(a); }
u128 FN(frsqest)(u128 a) { return spu_frsqest(a); }
u128 FN(gb)(u128 a)      { return spu_gb(a); }
u128 FN(fsm)(u128 a)     { return spu_fsm(a); }
u128 FN(csflt)(u128 a, int i8) { return spu_csflt(a, i8); }
u128 FN(cuflt)(u128 a, int i8) { return spu_cuflt(a, i8); }
u128 FN(cflts)(u128 a, int i8) { return spu_cflts(a, i8); }
u128 FN(shlqbii)(u128 a, int sh) { return spu_shlqbii(a, sh); }
u128 FN(rotqbii)(u128 a, int sh) { return spu_rotqbii(a, sh); }
'''

DRIVER = r'''
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#define SPU_LS_NO_PROBES
#include "spu_context.h"

extern u128 ref_frest(u128), sse_frest(u128);
extern u128 ref_frsqest(u128), sse_frsqest(u128);
extern u128 ref_gb(u128), sse_gb(u128);
extern u128 ref_fsm(u128), sse_fsm(u128);
extern u128 ref_csflt(u128, int), sse_csflt(u128, int);
extern u128 ref_cuflt(u128, int), sse_cuflt(u128, int);
extern u128 ref_cflts(u128, int), sse_cflts(u128, int);
extern u128 ref_shlqbii(u128, int), sse_shlqbii(u128, int);
extern u128 ref_rotqbii(u128, int), sse_rotqbii(u128, int);

static uint32_t rng = 0x12345678u;
static uint32_t nxt(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

int main(void) {
    uint32_t edges[] = {
        0u, 0x80000000u, 1u, 0x7FFFFFu, 0x800000u, 0x3F800000u, 0xBF800000u,
        0x4EFFFFFFu, 0x4F000000u, 0xCF000000u, 0xCF000001u, 0x7E800000u,
        0x7F000000u, 0x7F7FFFFFu, 0x7F800000u, 0xFF800000u, 0x7FC00000u,
        0xFFFFFFFFu, 0x7FFFFFFFu, 0x80000001u
    };
    int ne = (int)(sizeof(edges) / sizeof(edges[0]));
    for (int t = 0; t < 1000000; t++) {
        u128 a;
        for (int i = 0; i < 4; i++)
            a._u32[i] = (t < 4096) ? edges[(t >> (i * 2)) % ne] : nxt();
        int i8 = (t < 256) ? t : (int)(nxt() & 255);
        int sh = t & 15;
        u128 r1, r2;
        r1 = ref_frest(a);   r2 = sse_frest(a);   if (memcmp(&r1, &r2, 16)) { fprintf(stderr, "FAIL frest\n"); return 1; }
        r1 = ref_frsqest(a); r2 = sse_frsqest(a); if (memcmp(&r1, &r2, 16)) { fprintf(stderr, "FAIL frsqest\n"); return 1; }
        r1 = ref_gb(a);      r2 = sse_gb(a);      if (memcmp(&r1, &r2, 16)) { fprintf(stderr, "FAIL gb\n"); return 1; }
        r1 = ref_fsm(a);     r2 = sse_fsm(a);     if (memcmp(&r1, &r2, 16)) { fprintf(stderr, "FAIL fsm\n"); return 1; }
        r1 = ref_csflt(a, i8); r2 = sse_csflt(a, i8); if (memcmp(&r1, &r2, 16)) { fprintf(stderr, "FAIL csflt\n"); return 1; }
        r1 = ref_cuflt(a, i8); r2 = sse_cuflt(a, i8); if (memcmp(&r1, &r2, 16)) { fprintf(stderr, "FAIL cuflt\n"); return 1; }
        r1 = ref_cflts(a, i8); r2 = sse_cflts(a, i8); if (memcmp(&r1, &r2, 16)) { fprintf(stderr, "FAIL cflts\n"); return 1; }
        r1 = ref_shlqbii(a, sh); r2 = sse_shlqbii(a, sh); if (memcmp(&r1, &r2, 16)) { fprintf(stderr, "FAIL shlqbii sh=%d\n", sh); return 1; }
        r1 = ref_rotqbii(a, sh); r2 = sse_rotqbii(a, sh); if (memcmp(&r1, &r2, 16)) { fprintf(stderr, "FAIL rotqbii sh=%d\n", sh); return 1; }
    }
    printf("spu_helpers_sse: 9000000 bit-exact comparisons passed (frest/frsqest/gb/fsm/csflt/cuflt/cflts/shlqbii/rotqbii)\n");
    return 0;
}
'''

def main() -> int:
    inc = [f"-I{RUNTIME / 'runtime' / 'spu'}", f"-I{RUNTIME / 'include'}"]
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        (td / "ops.c").write_text(HARNESS)
        (td / "main.c").write_text(DRIVER)
        subprocess.check_call([str(CLANG), "/O2", "/c", "-DSPU_FLOAT_SCALAR", "-DSPU_QBI_SCALAR", "-DREF_SCALAR", *inc,
                               str(td / "ops.c"), f"/Fo{td / 'ref.obj'}"],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        subprocess.check_call([str(CLANG), "/O2", "/c", "-msse4.1", *inc,
                               str(td / "ops.c"), f"/Fo{td / 'sse.obj'}"],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        subprocess.check_call([str(CLANG), "/O2", *inc, str(td / "main.c"),
                               str(td / "ref.obj"), str(td / "sse.obj"), f"/Fe{td / 'test.exe'}"],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return subprocess.call([str(td / "test.exe")])

if __name__ == "__main__":
    sys.exit(main())
