// RSX vertex interpreter, vs_5_0. Semantics follow rsx_vp_decompiler.c in this
// tree (NV40 co-issued vector + scalar). No RPCS3 source. RSX_VP_INTERP_FETCH
// 1 reads attributes from t20/b2; 0 reads ATTR0..15. The program is the raw
// little-endian words at t21. TXL is 0, matching the D3D12 decompile calls,
// which pass a zero vertex-texture mask.

#ifndef RSX_VP_INTERP_FETCH
#define RSX_VP_INTERP_FETCH 1
#endif
#ifndef RSX_VP_INTERP_STEPS
#define RSX_VP_INTERP_STEPS 4096
#endif

cbuffer VPConst : register(b0) {
    float4 vp_c[512];
    float4 vp_posscale;
    float4 vp_posoffset;
};
ByteAddressBuffer rsx_vprog : register(t21);
cbuffer RSXVPInterp : register(b4) {
    uint vp_hdr;
    uint vp_pad1;
    uint vp_pad2;
    uint vp_pad3;
};

#if RSX_VP_INTERP_FETCH
ByteAddressBuffer rsx_vraw : register(t20);
cbuffer VFetch : register(b2) { uint4 vf_desc[16]; float4 vf_def[16]; };
uint vf_le32(uint a) {
    uint al = a & ~3u; uint sh = (a & 3u) * 8u;
    uint w0 = rsx_vraw.Load(al);
    if (sh == 0u) return w0;
    return (w0 >> sh) | (rsx_vraw.Load(al + 4u) << (32u - sh));
}
uint vf_be32(uint a) { uint w = vf_le32(a);
    return (w >> 24) | ((w >> 8) & 0xFF00u) | ((w << 8) & 0xFF0000u) | (w << 24); }
uint vf_be16(uint a) { uint w = vf_le32(a); return ((w & 0xFFu) << 8) | ((w >> 8) & 0xFFu); }
uint vf_u8(uint a) { return vf_le32(a) & 0xFFu; }
float vf_half(uint h) {
    uint sg = (h >> 15) & 1u, e = (h >> 10) & 0x1Fu, m = h & 0x3FFu; uint f;
    if (e == 0u) f = sg << 31;
    else if (e == 31u) f = (sg << 31) | 0x7F800000u | (m << 13);
    else f = (sg << 31) | ((e + 112u) << 23) | (m << 13);
    return asfloat(f);
}
float4 rsx_vfetch(uint k, uint vid) {
    float4 r = vf_def[k];
    uint4 d = vf_desc[k];
    if ((d.z & 0x100u) == 0u) return r;
    uint ty = d.z & 0xFu, n = (d.z >> 4) & 7u;
    uint a = d.x + vid * d.y;
    float c0 = r.x, c1 = r.y, c2 = r.z, c3 = r.w;
    if (ty == 6u) {
        uint w = vf_be32(a);
        int x = int(w & 0x7FFu), y = int((w >> 11) & 0x7FFu), z = int((w >> 22) & 0x3FFu);
        if (x & 0x400) x -= 0x800;
        if (y & 0x400) y -= 0x800;
        if (z & 0x200) z -= 0x400;
        c0 = float(x) / 1023.0; c1 = float(y) / 1023.0; c2 = float(z) / 511.0;
    } else {
        [unroll] for (uint i = 0; i < 4u; i++) {
            if (i < n) {
                float v;
                if (ty == 3u)      v = vf_half(vf_be16(a + 2u * i));
                else if (ty == 4u) v = float(vf_u8(a + i)) / 255.0;
                else if (ty == 1u) v = float(int(vf_be16(a + 2u * i) << 16) >> 16) / 32767.0;
                else if (ty == 5u) v = float(int(vf_be16(a + 2u * i) << 16) >> 16);
                else if (ty == 7u) v = float(vf_u8(a + i));
                else               v = asfloat(vf_be32(a + 4u * i));
                if (i == 0u) c0 = v; else if (i == 1u) c1 = v; else if (i == 2u) c2 = v; else c3 = v;
            }
        }
    }
    return float4(c0, c1, c2, c3);
}
struct VSInput { uint vid : SV_VertexID; };
#else
struct VSInput {
    float4 a0:ATTR0;  float4 a1:ATTR1;  float4 a2:ATTR2;  float4 a3:ATTR3;
    float4 a4:ATTR4;  float4 a5:ATTR5;  float4 a6:ATTR6;  float4 a7:ATTR7;
    float4 a8:ATTR8;  float4 a9:ATTR9;  float4 a10:ATTR10; float4 a11:ATTR11;
    float4 a12:ATTR12; float4 a13:ATTR13; float4 a14:ATTR14; float4 a15:ATTR15;
};
#endif

struct VSOutput {
    float4 pos:SV_Position; float4 col0:COLOR0; float4 col1:COLOR1;
    float4 fog:FOG;
    float4 t0:TEXCOORD0; float4 t1:TEXCOORD1; float4 t2:TEXCOORD2; float4 t3:TEXCOORD3;
    float4 t4:TEXCOORD4; float4 t5:TEXCOORD5; float4 t6:TEXCOORD6; float4 t7:TEXCOORD7;
    float4 t8:TEXCOORD8; float4 t9:TEXCOORD9;
};

float vp_mul1(float a, float b) { return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }
float4 vp_mul(float4 a, float4 b) {
    return float4(vp_mul1(a.x, b.x), vp_mul1(a.y, b.y), vp_mul1(a.z, b.z), vp_mul1(a.w, b.w));
}
float vp_dot3(float4 a, float4 b) { float4 m = vp_mul(a, b); return m.x + m.y + m.z; }
float vp_dot4(float4 a, float4 b) { float4 m = vp_mul(a, b); return m.x + m.y + m.z + m.w; }

float4 vp_swiz(float4 b, uint src) {
    uint sw = (src >> 8) & 3u, sz = (src >> 10) & 3u, sy = (src >> 12) & 3u, sx = (src >> 14) & 3u;
    return float4(b[sx], b[sy], b[sz], b[sw]);
}

int vp_addr(int4 a0, int4 a1, uint sel, uint sw) {
    int4 a = sel != 0u ? a1 : a0;
    return sw == 0u ? a.x : sw == 1u ? a.y : sw == 2u ? a.z : a.w;
}

bool vp_cc_pass(float cv, uint cond) {
    if (cond == 1u) return cv < 0.0;
    if (cond == 2u) return cv == 0.0;
    if (cond == 3u) return cv <= 0.0;
    if (cond == 4u) return cv > 0.0;
    if (cond == 5u) return cv != 0.0;
    if (cond == 6u) return cv >= 0.0;
    if (cond == 7u) return true;
    return false;
}

// One 17-bit source operand: register file, swizzle, then absolute, then negate.
// A macro, not a function, so v[] and r[] are not copied per call.
#define VP_SRC(DST, SRC, ABSBIT) { \
    uint _s = (SRC); uint _ty = _s & 3u; float4 _b = (float4)0; \
    if (_ty == 2u) _b = v[input_src & 15u]; \
    else if (_ty == 3u) { \
        uint _ix = const_src & 511u; \
        if (index_const != 0u) _ix = (uint)((int)const_src + vp_addr(a0, a1, addr_sel, addr_swz)) & 511u; \
        _b = vp_c[_ix]; \
    } else _b = r[(_s >> 2) & 31u]; \
    _b = vp_swiz(_b, _s); \
    if ((ABSBIT) != 0u) _b = abs(_b); \
    if (((_s >> 16) & 1u) != 0u) _b = -_b; \
    DST = _b; }

VSOutput main(VSInput input) {
    float4 v[16];
    float4 r[32];
    float4 o[16];
    float4 cc[2];
    int4 a0 = (int4)0;
    int4 a1 = (int4)0;
    [unroll] for (int i = 0; i < 16; i++) { v[i] = float4(0, 0, 0, 1); o[i] = float4(0, 0, 0, 1); }
    [unroll] for (int j = 0; j < 32; j++) r[j] = (float4)0;
    cc[0] = (float4)0; cc[1] = (float4)0;
    uint h = vp_hdr;
#if RSX_VP_INTERP_FETCH
    // Only the attributes the program reads, as the specialized fetch shader does. One loop,
    // so the fetch code is emitted once rather than sixteen times.
    uint imask = rsx_vprog.Load(h + 20u);
    [loop] for (uint k = 0; k < 16u; k++) if (((imask >> k) & 1u) != 0u) v[k] = rsx_vfetch(k, input.vid);
#else
    v[0]=input.a0; v[1]=input.a1; v[2]=input.a2; v[3]=input.a3;
    v[4]=input.a4; v[5]=input.a5; v[6]=input.a6; v[7]=input.a7;
    v[8]=input.a8; v[9]=input.a9; v[10]=input.a10; v[11]=input.a11;
    v[12]=input.a12; v[13]=input.a13; v[14]=input.a14; v[15]=input.a15;
#endif
    uint ioff = rsx_vprog.Load(h);
    uint count = rsx_vprog.Load(h + 4u);
    uint base = rsx_vprog.Load(h + 8u);
    uint bits = rsx_vprog.Load(h + 12u);
    uint flags = rsx_vprog.Load(h + 16u);
    int pc = 0;
    int cs[8];
    int csp = 0;
    [unroll] for (int s = 0; s < 8; s++) cs[s] = 0;
    [loop] for (int it = 0; it < RSX_VP_INTERP_STEPS && pc >= 0 && (uint)pc < count; it++) {
        uint at = ioff + (uint)pc * 16u;
        uint d0 = rsx_vprog.Load(at);
        uint d1 = rsx_vprog.Load(at + 4u);
        uint d2 = rsx_vprog.Load(at + 8u);
        uint d3 = rsx_vprog.Load(at + 12u);
        uint vec_op = (d1 >> 22) & 0x1Fu;
        uint sca_op = (d1 >> 27) & 0x1Fu;
        uint input_src = (d1 >> 8) & 0xFu;
        uint const_src = (d1 >> 12) & 0x3FFu;
        uint index_const = (d3 >> 1) & 1u;
        uint src0 = ((d2 >> 23) & 0x1FFu) | ((d1 & 0xFFu) << 9);
        uint src1 = (d2 >> 6) & 0x1FFFFu;
        uint src2 = ((d3 >> 21) & 0x7FFu) | ((d2 & 0x3Fu) << 11);
        uint dst_tmp = (d0 >> 15) & 0x3Fu;
        uint dst_out = (d3 >> 2) & 0x1Fu;
        uint sca_dst_tmp = (d3 >> 7) & 0x3Fu;
        uint vec_result = (d0 >> 30) & 1u;
        uint vmask = ((d3 >> 16) & 1u) | (((d3 >> 15) & 1u) << 1) | (((d3 >> 14) & 1u) << 2) | (((d3 >> 13) & 1u) << 3);
        uint smask = ((d3 >> 20) & 1u) | (((d3 >> 19) & 1u) << 1) | (((d3 >> 18) & 1u) << 2) | (((d3 >> 17) & 1u) << 3);
        uint cond = (d0 >> 10) & 7u;
        uint cond_test = (d0 >> 13) & 1u;
        uint csel = (d0 >> 25) & 1u;
        uint cswx = (d0 >> 8) & 3u, cswy = (d0 >> 6) & 3u, cswz = (d0 >> 4) & 3u, csww = (d0 >> 2) & 3u;
        uint4 csw = uint4(cswx, cswy, cswz, csww);
        int predicated = (cond_test != 0u && cond != 7u) ? 1 : 0;
        uint sat_bit = (d0 >> 26) & 1u;
        uint addr_sel = (d0 >> 24) & 1u;
        uint addr_swz = d0 & 3u;
        float4 A, B, C;
        VP_SRC(A, src0, (d0 >> 21) & 1u)
        VP_SRC(B, src1, (d0 >> 22) & 1u)
        VP_SRC(C, src2, (d0 >> 23) & 1u)
        int branched = 0;
        int next = pc + 1;
        if (vmask != 0u && vec_op != 0u) {
            if (vec_op == 0x0Du) {
                int4 ar = (int4)floor(A);
                [unroll] for (uint c = 0u; c < 4u; c++) if (vmask & (1u << c)) {
                    if (addr_sel != 0u) a1[c] = ar[c]; else a0[c] = ar[c];
                }
            } else if (vec_op <= 0x16u || vec_op == 0x19u) {
                float4 val = (float4)0;
                if (vec_op == 0x01u) val = A;
                else if (vec_op == 0x02u) val = vp_mul(A, B);
                else if (vec_op == 0x03u) val = A + C;
                else if (vec_op == 0x04u) val = vp_mul(A, B) + C;
                else if (vec_op == 0x05u) val = (float4)vp_dot3(A, B);
                else if (vec_op == 0x06u) val = (float4)(vp_dot3(A, B) + B.w);
                else if (vec_op == 0x07u) val = (float4)vp_dot4(A, B);
                else if (vec_op == 0x08u) val = float4(1, A.y * B.y, A.z, B.w);
                else if (vec_op == 0x09u) val = min(A, B);
                else if (vec_op == 0x0Au) val = max(A, B);
                else if (vec_op == 0x0Bu) val = float4(A.x < B.x, A.y < B.y, A.z < B.z, A.w < B.w);
                else if (vec_op == 0x0Cu) val = float4(A.x >= B.x, A.y >= B.y, A.z >= B.z, A.w >= B.w);
                else if (vec_op == 0x0Eu) val = frac(A);
                else if (vec_op == 0x0Fu) val = floor(A);
                else if (vec_op == 0x10u) val = float4(A.x == B.x, A.y == B.y, A.z == B.z, A.w == B.w);
                else if (vec_op == 0x11u) val = (float4)0;
                else if (vec_op == 0x12u) val = float4(A.x > B.x, A.y > B.y, A.z > B.z, A.w > B.w);
                else if (vec_op == 0x13u) val = float4(A.x <= B.x, A.y <= B.y, A.z <= B.z, A.w <= B.w);
                else if (vec_op == 0x14u) val = float4(A.x != B.x, A.y != B.y, A.z != B.z, A.w != B.w);
                else if (vec_op == 0x15u) val = float4(1, 1, 1, 1);
                else if (vec_op == 0x16u) val = sign(A);
                else val = (float4)0;
                if (sat_bit) val = saturate(val);
                uint cc_update = ((d0 >> 14) & 1u) && ((d0 >> 29) & 1u);
                bool ok[4];
                [unroll] for (uint c = 0u; c < 4u; c++)
                    ok[c] = predicated == 0 || vp_cc_pass(cc[csel][csw[c]], cond);
                [unroll] for (uint c = 0u; c < 4u; c++) if ((vmask & (1u << c)) != 0u && ok[c]) {
                    if (vec_result != 0u && dst_out != 0x1Fu) o[dst_out & 15u][c] = val[c];
                    if (dst_tmp != 0x3Fu) r[dst_tmp & 31u][c] = val[c];
                }
                // The decompiler stores the condition register last, testing it live per lane.
                if (cc_update != 0u) {
                    [unroll] for (uint c = 0u; c < 4u; c++) if ((vmask & (1u << c)) != 0u &&
                        (predicated == 0 || vp_cc_pass(cc[csel][csw[c]], cond))) cc[csel][c] = val[c];
                }
            }
        }
        uint flow = (sca_op == 0x08u || sca_op == 0x09u || sca_op == 0x0Au || sca_op == 0x0Bu ||
                     sca_op == 0x0Cu || sca_op == 0x11u || sca_op == 0x12u) ? 1u : 0u;
        if (smask != 0u && sca_op != 0u && flow == 0u && sca_op <= 0x10u && sca_op != 0x08u) {
            float4 C2;
            VP_SRC(C2, src2, (d0 >> 23) & 1u)
            float4 val = (float4)0;
            float x = C2.x;
            if (sca_op == 0x01u) val = C2;
            else if (sca_op == 0x02u) val = (float4)(1.0 / x);
            else if (sca_op == 0x03u) val = (float4)clamp(1.0 / x, 5.42101e-20, 1.884467e19);
            else if (sca_op == 0x04u) val = (float4)rsqrt(max(abs(x), 1e-10));
            else if (sca_op == 0x05u || sca_op == 0x0Eu) val = (float4)exp2(x);
            else if (sca_op == 0x06u || sca_op == 0x0Du) val = (float4)log2(max(abs(x), 1e-10));
            else if (sca_op == 0x0Fu) val = (float4)sin(x);
            else if (sca_op == 0x10u) val = (float4)cos(x);
            else if (sca_op == 0x07u) {
                float lit = (C2.x > 0.0) ? exp2(clamp(C2.w, -127.996, 127.996) * log2(max(C2.y, 1e-10))) : 0.0;
                val = float4(1.0, max(C2.x, 0.0), lit, 1.0);
            }
            if (sat_bit) val = saturate(val);
            [unroll] for (uint c = 0u; c < 4u; c++) if ((smask & (1u << c)) != 0u &&
                (predicated == 0 || vp_cc_pass(cc[csel][csw[c]], cond))) {
                if (vec_result == 0u && dst_out != 0x1Fu) o[dst_out & 15u][c] = val[c];
                if (sca_dst_tmp != 0x3Fu) r[sca_dst_tmp & 31u][c] = val[c];
            }
        }
        if (flow != 0u) {
            uint tgt = (((d0 >> 23) & 1u) << 9) | ((d2 & 0x3Fu) << 3) | ((d3 >> 29) & 7u);
            uint rel = tgt >= base ? tgt - base : tgt;
            int taken = 0;
            if (sca_op == 0x11u || sca_op == 0x12u) {
                uint bit = (bits >> ((d3 >> 23) & 0x1Fu)) & 1u;
                taken = bit == ((d3 >> 28) & 1u);
            } else if (cond == 7u) taken = 1;
            else if (cond == 0u) taken = 0;
            else taken = vp_cc_pass(cc[csel][cswx], cond) ? 1 : 0;
            branched = 1;
            if (sca_op == 0x08u || sca_op == 0x09u || sca_op == 0x11u) {
                pc = taken ? (int)rel : next;
            } else if (sca_op == 0x0Au || sca_op == 0x0Bu || sca_op == 0x12u) {
                if (taken) { cs[csp] = next; csp = min(csp + 1, 7); pc = (int)rel; }
                else pc = next;
            } else {
                if (taken) { if (csp > 0) { csp--; pc = cs[csp]; } else pc = -1; }
                else pc = next;
            }
        }
        if (!branched) pc = (d3 & 1u) ? -1 : next;
    }
    VSOutput Out;
    float4 p = o[0];
    Out.pos = float4(p.xyz * vp_posscale.xyz + p.w * vp_posoffset.xyz, p.w);
    if ((flags & 1u) != 0u) {
        if (vp_posoffset.w > 0.0) {
            float aw = abs(p.w);
            if (abs(abs(Out.pos.x) - vp_posoffset.w * aw) < 0.004 * aw) Out.pos.x = Out.pos.x < 0.0 ? -aw : aw;
        } else if (vp_posoffset.w < 0.0) {
            float aw = abs(p.w);
            if (abs(abs(Out.pos.x) - aw) < 0.004 * aw) Out.pos.x *= -vp_posoffset.w;
        }
    }
    Out.col0 = o[1]; Out.col1 = o[2]; Out.fog = o[5].xxxx;
    Out.t0=o[7]; Out.t1=o[8]; Out.t2=o[9]; Out.t3=o[10];
    Out.t4=o[11]; Out.t5=o[12]; Out.t6=o[13]; Out.t7=o[14];
    Out.t8=o[15]; Out.t9=o[6];
    return Out;
}
