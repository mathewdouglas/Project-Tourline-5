/* rsx_texture_decode_rows (row bands, decoded on several threads by the D3D12 backend) must give
 * exactly what rsx_texture_decode gives: every format, swizzled and linear, packed and padded pitch.
 * Synthetic bytes only. Run through tex_decode_bands.py. */
#include "rsx_texture_layout.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void) {
    /* RSX formats: plain + swizzled (LN bit 0x20 set = linear). */
    const u32 fmts[] = {0x81,0x82,0x83,0x84,0x85,0x86,0x87,0x88,0x8B,0x8F,0x90,0x9A,0x9B,0x9E,0x9F,
                        0xA1,0xA2,0xA3,0xA4,0xA5,0xA6,0xA7,0xA8,0xAB,0xAF,0xB0,0xBA,0xBB,0xBE,0xBF};
    const u32 sizes[][2] = {{256,256},{512,128},{64,1024},{1024,1024},{8,8}};
    int bad = 0, n = 0;
    srand(1);
    for (unsigned f = 0; f < sizeof fmts / sizeof *fmts; f++)
    for (unsigned s = 0; s < 5; s++) {
        u32 w = sizes[s][0], h = sizes[s][1];
        rsx_tex_layout tl; rsx_texture_layout(fmts[f], w, h, &tl);
        size_t sb = (size_t)tl.row_bytes * tl.rows + 64;
        u32 rows = rsx_texture_decode_nrows(h, &tl);
        for (int pp = 0; pp < 2; pp++) {
            u32 pitch = pp ? ((tl.dst_row_bytes + 255) & ~255u) : tl.dst_row_bytes;
            size_t db = (size_t)pitch * rows;
            u8* src = malloc(sb); for (size_t i = 0; i < sb; i++) src[i] = (u8)rand();
            u8* a = calloc(1, db), * b = calloc(1, db);
            rsx_texture_decode(a, pitch, src, w, h, &tl, 0);
            u32 band = rows / 7 + 1;
            for (u32 y = 0; y < rows; y += band) rsx_texture_decode_rows(b, pitch, src, w, h, &tl, 0, y, y + band);
            n++;
            if (memcmp(a, b, db)) { bad++; printf("MISMATCH fmt %02X %ux%u pitch %u\n", fmts[f], w, h, pitch); }
            free(src); free(a); free(b);
        }
    }
    printf("%d cases, %d mismatches\n", n, bad);
    return bad != 0;
}
