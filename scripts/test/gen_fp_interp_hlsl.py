"""Generate the fragment-interpreter HLSL embedded in the runtime patch.

The text is original. It follows the behaviour of libs/video/rsx_fp_decompiler.c
and the backend rewrites that run after it (alpha test, fog, unnormalised
scale, sRGB, shadow compare, dither). It is not taken from RPCS3.
"""
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
# INTERP_OUT_DIR: where to write (default scratch/runtime-src/libs/video).
OUT = Path(os.environ.get("INTERP_OUT_DIR") or ROOT / "scratch" / "runtime-src" / "libs" / "video") / "rsx_fp_interp_hlsl.inc"


def texture_block():
    # ps_5_1 indexes texture descriptor ranges. Samplers stay individual
    # static samplers: a sampler array wants a descriptor table, and the
    # engine root signature binds s0-s15 as static samplers.
    lines = [
        "// RSX_INTERP_SHORT (default 1; RSX_SHADER_INTERP_SHORT=0 off): plain samples go through the biased",
        "// sample site with bias 0, identity swizzles and unmodified operands skip that work.",
        "// RSX_INTERP_NODISC: the host picks this variant for a program without KIL drawn with the alpha test",
        "// off; it has no discard, so the GPU can depth-test before running it.",
        "// RSX_INTERP_EARLYZ: the host picks this variant for a draw that can discard but writes neither depth",
        "// nor stencil; depth is then tested before the shader runs (a discarded pixel has nothing to undo).",
        "#ifndef RSX_INTERP_EARLYZ",
        "#define RSX_INTERP_EARLYZ 0",
        "#endif",
        "#ifndef RSX_INTERP_NODISC",
        "#define RSX_INTERP_NODISC 0",
        "#endif",
        "#ifndef RSX_INTERP_SHORT",
        "#define RSX_INTERP_SHORT 1",
        "#endif",
        "Texture2D rsx_i2d[16] : register(t0);",
        "TextureCube rsx_icu[16] : register(t32);",
        "Texture3D rsx_i3d[16] : register(t48);",
    ]
    for i in range(16):
        lines.append(f"SamplerState rsx_is{i} : register(s{i});")
    lines.append("")

    def emit_fn(name, tex, coord_type):
        lines.append(f"float4 {name}(uint unit, uint samp, {coord_type} uv, uint kind, float lod) {{")
        lines.append("    unit &= 15u;")
        lines.append("    switch (samp) {")
        for s in range(16):
            lines.append(f"    case {s}u:")
            lines.append("#if RSX_INTERP_SHORT")
            lines.append(f"        if (kind != 2u) return {tex}[unit].SampleBias(rsx_is{s}, uv, kind == 1u ? lod : 0.0);")
            lines.append("#else")
            lines.append(f"        if (kind == 0u) return {tex}[unit].Sample(rsx_is{s}, uv);")
            lines.append(f"        if (kind == 1u) return {tex}[unit].SampleBias(rsx_is{s}, uv, lod);")
            lines.append("#endif")
            lines.append(f"        return {tex}[unit].SampleLevel(rsx_is{s}, uv, lod);")
        lines.append("    default: break; }")
        lines.append("    return (float4)0;")
        lines.append("}")

    emit_fn("rsx_i_s2d", "rsx_i2d", "float2")
    emit_fn("rsx_i_scu", "rsx_icu", "float3")
    emit_fn("rsx_i_s3d", "rsx_i3d", "float3")
    lines.append("float rsx_i_load2d(uint unit, int2 q) {")
    lines.append("    uint w, h;")
    lines.append("    unit &= 15u;")
    lines.append("    rsx_i2d[unit].GetDimensions(w, h);")
    lines.append("    if (q.x < 0 || q.y < 0 || q.x >= (int)w || q.y >= (int)h) return 1.0;")
    lines.append("    return rsx_i2d[unit].Load(int3(q, 0)).x;")
    lines.append("}")
    lines.append("void rsx_i_dim2d(uint unit, out uint w, out uint h) {")
    lines.append("    unit &= 15u;")
    lines.append("    rsx_i2d[unit].GetDimensions(w, h);")
    lines.append("}")
    return "\n".join(lines) + "\n"


BODY = r'''
// RSX fragment interpreter (ps_5_1, so texture and sampler arrays index). Profile gates the backend epilogue so a
// harness can compare against the decompiler alone. RSX_INTERP_MRT / RSX_INTERP_SAMPLE
// select the output signature and sample-rate interpolants.

#ifndef RSX_FP_INTERP_STEPS
#define RSX_FP_INTERP_STEPS 2048
#endif
#ifndef RSX_INTERP_MRT
#define RSX_INTERP_MRT 0
#endif
#ifndef RSX_INTERP_SAMPLE
#define RSX_INTERP_SAMPLE 0
#endif
/* 1 (the default) declares TEXCOORD8/9 and SV_IsFrontFace. The engine compiles
 * a second entry with this set to 0 for programs that never read those inputs,
 * so a vertex shader that does not write them still links. */
#ifndef RSX_INTERP_FULLIN
#define RSX_INTERP_FULLIN 1
#endif
#ifndef RSX_INTERP_TARGETS
#define RSX_INTERP_TARGETS 4
#endif

#if RSX_INTERP_SAMPLE
#define RSX_IL sample
#else
#define RSX_IL
#endif

struct RSXInterpIn {
    float4 position : SV_POSITION;
    RSX_IL float4 col0 : COLOR0;
    RSX_IL float4 col1 : COLOR1;
    RSX_IL float4 fog : FOG;
    RSX_IL float4 tc0 : TEXCOORD0;
    RSX_IL float4 tc1 : TEXCOORD1;
    RSX_IL float4 tc2 : TEXCOORD2;
    RSX_IL float4 tc3 : TEXCOORD3;
    RSX_IL float4 tc4 : TEXCOORD4;
    RSX_IL float4 tc5 : TEXCOORD5;
    RSX_IL float4 tc6 : TEXCOORD6;
    RSX_IL float4 tc7 : TEXCOORD7;
#if RSX_INTERP_FULLIN
    RSX_IL float4 tc8 : TEXCOORD8;
    RSX_IL float4 tc9 : TEXCOORD9;
    bool front : SV_IsFrontFace;
#endif
};

cbuffer PSConstants : register(b1) {
    float4 rsx_texscale[4];
    float4 rsx_alphatest;
    float4 fp_constants[256];
    float4 rsx_fogstate;
    float4 rsx_texexpand[4];
    float4 rsx_texscale_hi[6];
};
cbuffer RSXInterpRoot : register(b3) { uint rsx_ip_hdr; uint rsx_ip_pad0; uint rsx_ip_pad1; uint rsx_ip_pad2; };
ByteAddressBuffer rsx_iprog : register(t16);

// Header at rsx_ip_hdr: instr_off, instr_count, flags, shape, unnorm, srgb,
// shadow, samp0, samp1. Flags: half, target count in bits 1-2 (count-1),
// depth (unused), atest, fog, wpos scale, final dither, lg2 abs, game dither,
// srgb write.

float4 rsx_i_swz(float4 v, uint s) {
    float c0 = (s & 3u) == 0u ? v.x : (s & 3u) == 1u ? v.y : (s & 3u) == 2u ? v.z : v.w;
    uint s1 = (s >> 2) & 3u, s2 = (s >> 4) & 3u, s3 = (s >> 6) & 3u;
    float c1 = s1 == 0u ? v.x : s1 == 1u ? v.y : s1 == 2u ? v.z : v.w;
    float c2 = s2 == 0u ? v.x : s2 == 1u ? v.y : s2 == 2u ? v.z : v.w;
    float c3 = s3 == 0u ? v.x : s3 == 1u ? v.y : s3 == 2u ? v.z : v.w;
    return float4(c0, c1, c2, c3);
}

float rsx_i_cmp1(float a, uint op) {
    if (op == 0u) return a == 0.0 ? 1.0 : 0.0;
    if (op == 1u) return a >= 0.0 ? 1.0 : 0.0;
    if (op == 2u) return a <= 0.0 ? 1.0 : 0.0;
    if (op == 3u) return a != 0.0 ? 1.0 : 0.0;
    if (op == 4u) return a > 0.0 ? 1.0 : 0.0;
    return a < 0.0 ? 1.0 : 0.0;
}
float4 rsx_i_cmp4(float4 a, uint op) {
    return float4(rsx_i_cmp1(a.x, op), rsx_i_cmp1(a.y, op), rsx_i_cmp1(a.z, op), rsx_i_cmp1(a.w, op));
}

float4 rsx_i_fog(float raw) {
    float c = raw;
    int m = (int)rsx_fogstate.z;
    if (m == 0) return raw.xxxx;
    if (m >= 4) c = abs(c);
    float t = dot(float2(c, 1), rsx_fogstate.yx);
    float f = 0;
    if (m == 1 || m == 4) f = t - 1;
    else if (m == 2 || m == 5) f = exp((t - 1.5) * 11.084);
    else { t = (t - 1.5) * 4.709; f = exp(-t * t); }
    return float4(raw, saturate(f), 0, 0);
}

float4 rsx_i_s2l(float4 s) {
    float3 c = saturate(s.rgb);
    s.rgb = lerp(c / 12.92, pow((c + 0.055) / 1.055, (float3)2.4), step((float3)0.04045, c));
    return s;
}
float3 rsx_i_l2s(float3 v) {
    float3 c = saturate(v);
    return lerp(c * 12.92, 1.055 * pow(c, (float3)(1.0 / 2.4)) - 0.055, step((float3)0.0031308, c));
}

uint rsx_i_pk4ub(float4 v) {
    uint4 b = (uint4)round(saturate(v) * 255.0);
    return b.x | (b.y << 8) | (b.z << 16) | (b.w << 24);
}
float4 rsx_i_up4ub(float v) {
    uint u = asuint(v);
    return float4(u & 255u, (u >> 8) & 255u, (u >> 16) & 255u, (u >> 24) & 255u) / 255.0;
}
uint rsx_i_pk4b(float4 v) {
    uint4 b = (uint4)((int4)round(clamp(v, -1.0, 1.0) * 127.0)) & 255u;
    return b.x | (b.y << 8) | (b.z << 16) | (b.w << 24);
}
float4 rsx_i_up4b(float v) {
    uint u = asuint(v);
    int4 b = int4(u & 255u, (u >> 8) & 255u, (u >> 16) & 255u, (u >> 24) & 255u);
    b = (b << 24) >> 24;
    return clamp((float4)b / 127.0, -1.0, 1.0);
}
uint rsx_i_pk2us(float4 v) {
    uint2 b = (uint2)round(saturate(v.xy) * 65535.0);
    return b.x | (b.y << 16);
}
float4 rsx_i_up2us(float v) {
    uint u = asuint(v);
    return float4(u & 65535u, u >> 16, u & 65535u, u >> 16) / 65535.0;
}
uint rsx_i_pk2h(float4 v) { return f32tof16(v.x) | (f32tof16(v.y) << 16); }
float4 rsx_i_up2h(float v) {
    uint u = asuint(v);
    return float4(f16tof32(u & 65535u), f16tof32(u >> 16), f16tof32(u & 65535u), f16tof32(u >> 16));
}
float4 rsx_i_lin2srgb(float4 cl) {
    cl = saturate(cl);
    float4 low = cl * 12.92;
    float4 high = 1.055 * pow(cl, (float4)(1.0 / 2.4)) - 0.055;
    return saturate(cl < 0.0031308 ? low : high);
}
float4 rsx_i_srgb2lin(float4 cs) {
    float4 a = cs / 12.92;
    float4 b = pow(max(cs + 0.055, (float4)0.0) / 1.055, (float4)2.4);
    return cs > 0.04045 ? b : a;
}

float rsx_i_shcmp(float depth, float reference, uint func) {
    if (func == 0u) return 0.0;
    if (func == 1u) return depth < reference ? 1.0 : 0.0;
    if (func == 2u) return depth == reference ? 1.0 : 0.0;
    if (func == 3u) return depth <= reference ? 1.0 : 0.0;
    if (func == 4u) return depth > reference ? 1.0 : 0.0;
    if (func == 5u) return depth != reference ? 1.0 : 0.0;
    if (func == 6u) return depth >= reference ? 1.0 : 0.0;
    return 1.0;
}
float rsx_i_shb(uint unit, float2 p, float r, int w, int h, uint func) {
    float2 i = floor(p);
    float2 f = p - i;
    int2 q = (int2)i;
    float4 d = float4(rsx_i_load2d(unit, q), rsx_i_load2d(unit, q + int2(1, 0)),
                      rsx_i_load2d(unit, q + int2(0, 1)), rsx_i_load2d(unit, q + int2(1, 1)));
    float4 l = float4(rsx_i_shcmp(d.x, r, func), rsx_i_shcmp(d.y, r, func),
                      rsx_i_shcmp(d.z, r, func), rsx_i_shcmp(d.w, r, func));
    return lerp(lerp(l.x, l.y, f.x), lerp(l.z, l.w, f.x), f.y);
}

float4 rsx_i_modify(float4 v, uint absb, uint prec, uint neg) {
    if (prec >= 1u && prec <= 4u) {
        if (prec == 4u) v = saturate(v);
        if (absb) v = abs(v);
        if (prec != 4u) {
            float b = prec == 1u ? 65504.0 : prec == 2u ? 2.0 : 1.0;
            v = clamp(v, -b, b);
        }
    } else if (absb) v = abs(v);
    if (neg) v = -v;
    return v;
}

float4 rsx_i_unnorm(float4 c, uint unit, uint shape, uint unnorm) {
    if (shape != 0u || (unnorm & (1u << unit)) == 0u) return c;
    if (unit < 4u) {
        c.xy = c.xy * rsx_texscale[unit].xy + rsx_texscale[unit].zw;
    } else {
        uint pair = (unit - 4u) >> 1;
        float2 sc = (unit & 1u) ? rsx_texscale_hi[pair].zw : rsx_texscale_hi[pair].xy;
        c.xy = c.xy * sc;
    }
    return c;
}

// Depth-copy unit (RSX_FP depth read as a colour texture): the 24-bit depth arrives as bytes A = high, R = middle,
// G = low (B is stencil), then goes through the unit's crossbar. Matches the specialized rsx_dz<u> wrapper.
float rsx_i_pick(float4 v, uint i) {
    return i == 0u ? v.x : i == 1u ? v.y : i == 2u ? v.z : i == 3u ? v.w : (i == 5u ? 1.0 : 0.0);
}
float4 rsx_i_dz(float4 s, uint ds) {
    float d = floor(saturate(s.x) * 16777215.0 + 0.5);
    float hi = floor(d / 65536.0);
    float mid = floor((d - hi * 65536.0) / 256.0);
    float lo = d - hi * 65536.0 - mid * 256.0;
    float4 v = float4(mid, lo, 0.0, hi) / 255.0;
    return float4(rsx_i_pick(v, ds & 7u), rsx_i_pick(v, (ds >> 3) & 7u),
                  rsx_i_pick(v, (ds >> 6) & 7u), rsx_i_pick(v, (ds >> 9) & 7u));
}

// Unit whose mip levels are stacked in one render target (TXL only; matches the specialized rsx_mip<u>).
float2 rsx_i_mipuv(uint unit, float2 uv, float l) {
    float k = exp2(-l);
    float2 e = float2(0.0008, 0.0014) * exp2(l);
    uv = clamp(uv, e, 1.0 - e);
    float2 sc = rsx_texscale[unit].xy;
    float2 o = rsx_texscale[unit].zw;
    return float2(uv.x * sc.x * k + o.x, o.y + sc.y * (2.0 - 2.0 * k) + uv.y * sc.y * k);
}
float4 rsx_i_mip(uint unit, uint samp, float2 uv, float lod, uint gamma) {
    float l = clamp(lod, 0.0, 5.0);
    float l0 = floor(l);
    float l1 = min(l0 + 1.0, 5.0);
    float4 a = rsx_i_s2d(unit, samp, rsx_i_mipuv(unit, uv, l0), 2u, 0.0);
    float4 b = rsx_i_s2d(unit, samp, rsx_i_mipuv(unit, uv, l1), 2u, 0.0);
    if (gamma != 0u) { a = rsx_i_s2l(a); b = rsx_i_s2l(b); }
    return lerp(a, b, l - l0);
}

void rsx_i_atest(float a) {
    if (rsx_alphatest.x > 0.5) {
        float r = rsx_alphatest.y;
        int f = (int)rsx_alphatest.z;
        bool p = true;
        if (f == 0) p = false;
        else if (f == 1) p = a < r;
        else if (f == 2) p = a == r;
        else if (f == 3) p = a <= r;
        else if (f == 4) p = a > r;
        else if (f == 5) p = a != r;
        else if (f == 6) p = a >= r;
#if RSX_INTERP_PROBE < 3 && !RSX_INTERP_NODISC
        if (!p) discard;
#endif
    }
}

// NaN guard that the HLSL optimizer cannot fold: (v == v) is assumed true under the default floating-point
// model, so the guard was silently dropped from some compiled layouts and a NaN reached the target (the specialized
// shader writes 0 there). Compares the bit pattern instead: NaN is exponent all ones with a non-zero mantissa.
float4 rsx_i_nan0(float4 v) {
    uint4 u = asuint(v) & 0x7FFFFFFFu;
    return (u > 0x7F800000u) ? (float4)0 : v;
}

// Register-file size class (the host picks the smallest that covers the program's registers and exports).
#ifndef RSX_INTERP_REGS
#define RSX_INTERP_REGS 48
#endif
#define RSX_INTERP_REGM ((uint)RSX_INTERP_REGS - 1u)

// RSX_INTERP_LEGACY (RSX_SHADER_INTERP_LEGACY=1): separate full/half register arrays, the texture
// coordinates rebuilt into an array every step, seven control-stack arrays. Otherwise one register file
// indexed (register * 2 + half), coordinates gathered once, and a two-array control stack.
#ifndef RSX_INTERP_LEGACY
#define RSX_INTERP_LEGACY 0
#endif
// RSX_INTERP_WMASK (default 0; RSX_SHADER_INTERP_WMASK=1 on): masked register writes, see the write path.
// Measured: interpreted draw time 11.5 ms/frame with both, 11.6 with WMASK alone, 11.7 with neither;
// within run-to-run spread, so both stay opt-in.
#ifndef RSX_INTERP_WMASK
#define RSX_INTERP_WMASK 0
#endif
// RSX_INTERP_SWITCH (default 1; RSX_SHADER_INTERP_SWITCH=0 off): the opcode chain as a switch. DXC kept the
// chain as compares; as a switch, forced interpreter: GPU 13.1 -> 12.4 ms, 41.1 -> 43.2 FPS.
#ifndef RSX_INTERP_SWITCH
#define RSX_INTERP_SWITCH 1
#endif
// RSX_INTERP_FLAT (default 0; RSX_SHADER_INTERP_FLAT=1 on): the register file as one float array.
#ifndef RSX_INTERP_FLAT
#define RSX_INTERP_FLAT 0
#endif
// RSX_INTERP_NOCLEAR (default 1; RSX_SHADER_INTERP_NOCLEAR=0 off): instead of zero-filling the register
// file for every pixel, a 96-bit mask in registers records which entries were written; an unwritten
// entry reads as zero. The clear was about 2.7 ms of a 13.9 ms frame with every draw interpreted.
// Not combined with RSX_INTERP_FLAT (that layout keeps the clear).
#ifndef RSX_INTERP_NOCLEAR
#define RSX_INTERP_NOCLEAR 1
#endif
#if RSX_INTERP_FLAT || RSX_INTERP_LEGACY
#undef RSX_INTERP_NOCLEAR
#define RSX_INTERP_NOCLEAR 0
#endif
bool rsx_wb_test(uint3 w, uint ri) {
    uint x = ri < 32u ? w.x : ri < 64u ? w.y : w.z;
    return ((x >> (ri & 31u)) & 1u) != 0u;
}
uint3 rsx_wb_set(uint3 w, uint ri) {
    uint b = 1u << (ri & 31u);
    if (ri < 32u) w.x |= b; else if (ri < 64u) w.y |= b; else w.z |= b;
    return w;
}
#if RSX_INTERP_LEGACY
#define RSX_IR(s, i) ((s) != 0u ? h[i] : r[i])
#define RSX_IW(s, i, val) { if ((s) != 0u) h[i] = (val); else r[i] = (val); }
#else
#if RSX_INTERP_FLAT
// One float per component: element ((register * 2 + half) * 4 + component). A temp source applies
// its swizzle as the load index and a masked write is one store per enabled component.
#define RSX_IRB(b) float4(rff[(b)], rff[(b) + 1u], rff[(b) + 2u], rff[(b) + 3u])
#define RSX_IR(s, i) RSX_IRB(((i) * 2u + (s)) * 4u)
#define RSX_IWC(ri, c, val) { rff[(ri) * 4u + (c)] = (val); }
#define RSX_IW(s, i, val) { uint wb_ = ((i) * 2u + (s)) * 4u; rff[wb_] = (val).x; rff[wb_ + 1u] = (val).y; \
                            rff[wb_ + 2u] = (val).z; rff[wb_ + 3u] = (val).w; }
#elif RSX_INTERP_NOCLEAR
#define RSX_IR(s, i) (rsx_wb_test(wb, (i) * 2u + (s)) ? rf[(i) * 2u + (s)] : (float4)0)
// A component write to an entry not yet written zeroes the entry first.
#define RSX_IWC(ri, c, val) { if (!rsx_wb_test(wb, ri)) { rf[ri] = (float4)0; wb = rsx_wb_set(wb, ri); } \
                              if ((c) == 0u) rf[ri].x = (val); else if ((c) == 1u) rf[ri].y = (val); \
                              else if ((c) == 2u) rf[ri].z = (val); else rf[ri].w = (val); }
#define RSX_IW(s, i, val) { rf[(i) * 2u + (s)] = (val); wb = rsx_wb_set(wb, (i) * 2u + (s)); }
#else
#define RSX_IR(s, i) rf[(i) * 2u + (s)]
#define RSX_IWC(ri, c, val) { if ((c) == 0u) rf[ri].x = (val); else if ((c) == 1u) rf[ri].y = (val); \
                              else if ((c) == 2u) rf[ri].z = (val); else rf[ri].w = (val); }
#define RSX_IW(s, i, val) { rf[(i) * 2u + (s)] = (val); }
#endif
#endif

uint rsx_i_expidx(uint half, uint target) {
    uint idx = 0;
    if (half != 0u) {
        if (target == 1u) idx = 4u;
        else if (target == 2u) idx = 6u;
        else if (target == 3u) idx = 8u;
    } else {
        if (target == 1u) idx = 2u;
        else if (target == 2u) idx = 3u;
        else if (target == 3u) idx = 4u;
    }
    return min(idx, RSX_INTERP_REGM);
}

// Probe 7 (measurement only): the entry point below is replaced by one whose only input is SV_POSITION.
#if defined(RSX_INTERP_PROBE) && RSX_INTERP_PROBE == 7
#define main rsx_i_main_unused
#endif
#if RSX_INTERP_MRT
struct RSXInterpOut {
    float4 c0 : SV_TARGET0;
#if RSX_INTERP_TARGETS > 1
    float4 c1 : SV_TARGET1;
#endif
#if RSX_INTERP_TARGETS > 2
    float4 c2 : SV_TARGET2;
#endif
#if RSX_INTERP_TARGETS > 3
    float4 c3 : SV_TARGET3;
#endif
};
#if RSX_INTERP_EARLYZ
[earlydepthstencil]
#endif
RSXInterpOut main(RSXInterpIn input) {
#else
#if RSX_INTERP_EARLYZ
[earlydepthstencil]
#endif
float4 main(RSXInterpIn input) : SV_TARGET {
#endif
#if RSX_INTERP_LEGACY
    float4 r[RSX_INTERP_REGS];
    float4 h[RSX_INTERP_REGS];
#else
#if RSX_INTERP_FLAT
    float rff[RSX_INTERP_REGS * 8];
#else
    float4 rf[RSX_INTERP_REGS * 2];
#endif
    uint3 wb = 0;
#endif
    float4 cc0 = 0, cc1 = 0;
#if defined(RSX_INTERP_PROBE) && RSX_INTERP_PROBE == 5
#if RSX_INTERP_MRT
    { RSXInterpOut po = (RSXInterpOut)0; return po; }
#else
    return (float4)0;
#endif
#endif
    uint hdr = rsx_ip_hdr;
    // Header word 11: how many registers the program can read (0 = all 48).
    uint nregs = rsx_iprog.Load(hdr + 44u);
    if (nregs == 0u || nregs > (uint)RSX_INTERP_REGS) nregs = (uint)RSX_INTERP_REGS;
#if defined(RSX_INTERP_PROBE) && RSX_INTERP_PROBE == 6
    nregs = 0u;
#endif
#if RSX_INTERP_LEGACY
    [loop] for (uint zi = 0u; zi < nregs; zi++) { r[zi] = 0; h[zi] = 0; }
#else
#if RSX_INTERP_FLAT
    [loop] for (uint zi = 0u; zi < nregs * 8u; zi++) rff[zi] = 0;
#elif !RSX_INTERP_NOCLEAR
    [loop] for (uint zi = 0u; zi < nregs * 2u; zi++) rf[zi] = 0;
#endif
#endif
    uint instr_off = rsx_iprog.Load(hdr + 0u);
    uint instr_count = rsx_iprog.Load(hdr + 4u);
    uint flags = rsx_iprog.Load(hdr + 8u);
    uint shape = rsx_iprog.Load(hdr + 12u);
    uint unnorm = rsx_iprog.Load(hdr + 16u);
    uint srgb = rsx_iprog.Load(hdr + 20u);
    uint shadow = rsx_iprog.Load(hdr + 24u);
    uint samp0 = rsx_iprog.Load(hdr + 28u);
    uint samp1 = rsx_iprog.Load(hdr + 32u);
    uint dzword = rsx_iprog.Load(hdr + 36u);   // bits 0-3 depth-copy units, 8-19 crossbar
    uint mipmask = rsx_iprog.Load(hdr + 40u);   // bits 0-3 stacked-mip units
    uint half = flags & 1u;
    uint lg2abs = (flags & 256u) != 0u;
    // RSX_SHADER_INTERP_PROBE (measurement only, renders wrong): 1 = texture fetches return 0.5,
    // 2 = no instruction is interpreted (fixed cost of an interpreted pixel), 3 = no discard, 4 = 2 and 3,
    // 5 = return at once (pipeline and draw cost only), 6 = 4 without the register clear.
#ifndef RSX_INTERP_PROBE
#define RSX_INTERP_PROBE 0
#endif
#if RSX_INTERP_PROBE == 2 || RSX_INTERP_PROBE == 4 || RSX_INTERP_PROBE == 6
    instr_count = 0u;
#endif
    float4 wpos = input.position;
    if ((flags & 64u) != 0u)
        wpos *= float4(rsx_fogstate.w > 0.0 ? rsx_fogstate.w : 1.0,
                       rsx_alphatest.w > 0.0 ? rsx_alphatest.w : 1.0, 1, 1);
#if RSX_INTERP_LEGACY
    uint stk_else[8], stk_end[8], stk_kind[8], stk_i[8], stk_step[8], stk_endn[8], stk_body[8];
#define SK_ELSE(i) stk_else[i]
#define SK_END(i) stk_end[i]
#define SK_KIND(i) stk_kind[i]
#define SK_BODY(i) stk_body[i]
#define SK_I(i) stk_i[i]
#define SK_STEP(i) stk_step[i]
#define SK_ENDN(i) stk_endn[i]
#else
    uint4 ska[8], skb[8];   // (else, end, kind, body), (counter, step, end count, -)
#define SK_ELSE(i) ska[i].x
#define SK_END(i) ska[i].y
#define SK_KIND(i) ska[i].z
#define SK_BODY(i) ska[i].w
#define SK_I(i) skb[i].x
#define SK_STEP(i) skb[i].y
#define SK_ENDN(i) skb[i].z
#endif
    uint sp = 0, pc = 0, steps = 0, killed = 0;
#if !RSX_INTERP_LEGACY
#if RSX_INTERP_FULLIN
    float4 tc8v = input.tc8;
    float4 tc9v = input.tc9;
    float4 face = input.front ? (float4)1 : (float4)(-1);
#else
    float4 tc8v = 0;
    float4 tc9v = 0;
    float4 face = 0;
#endif
    float4 tc[10] = { input.tc0, input.tc1, input.tc2, input.tc3, input.tc4,
                      input.tc5, input.tc6, input.tc7, tc8v, tc9v };
#endif
    [loop]
    while (pc < instr_count && steps < RSX_FP_INTERP_STEPS) {
        steps++;
        [loop]
        for (int g = 0; g < 16; g++) {
            if (sp > 0u && SK_KIND(sp - 1u) == 0u && SK_ELSE(sp - 1u) == pc) {
                pc = SK_END(sp - 1u);
                continue;
            }
            if (sp > 0u && SK_END(sp - 1u) == pc) {
                if (SK_KIND(sp - 1u) == 1u) {
                    uint ni = SK_I(sp - 1u) + SK_STEP(sp - 1u);
                    if (ni < SK_ENDN(sp - 1u)) { SK_I(sp - 1u) = ni; pc = SK_BODY(sp - 1u); continue; }
                }
                sp--;
                continue;
            }
            break;
        }
        if (pc >= instr_count || steps >= RSX_FP_INTERP_STEPS) break;
        uint4 iw = rsx_iprog.Load4(instr_off + pc * 16u);
        uint next = pc + 1u;
        uint op = iw.x & 63u;
        uint dest = (iw.x >> 6) & 63u;
        uint dhalf = (iw.x >> 12) & 1u;
        uint nowrite = (iw.x >> 13) & 1u;
        uint mask = (iw.x >> 14) & 15u;
        uint osat = (iw.x >> 18) & 1u;
        uint scale = (iw.x >> 19) & 7u;
        uint pred = (iw.x >> 22) & 3u;
        uint cmpop = (iw.x >> 24) & 7u;
        uint ccread = (iw.x >> 27) & 1u;
        uint setcond = (iw.x >> 28) & 1u;
        uint ccwrite = (iw.x >> 29) & 1u;
        uint persp = (iw.x >> 30) & 1u;
        uint flow = (iw.x >> 31) & 1u;
        if (flow) {
            uint else_pc = iw.y & 0xFFFFu;
            uint end_pc = iw.y >> 16;
            uint init = iw.z & 255u;
            uint endn = (iw.z >> 8) & 255u;
            uint step = (iw.z >> 16) & 255u;
            uint fcsw = iw.w & 255u;
            float ccv = (ccread != 0u ? rsx_i_swz(cc1, fcsw) : rsx_i_swz(cc0, fcsw)).x;
            bool cond = pred == 0u ? true : pred == 1u ? false : rsx_i_cmp1(ccv, cmpop) != 0.0;
            if (op == 2u) {
                if (cond) {
                    if (sp < 8u) {
                        SK_KIND(sp) = 0u;
                        SK_ELSE(sp) = else_pc == 0xFFFFu ? 0xFFFFFFFFu : else_pc;
                        SK_END(sp) = end_pc;
                        sp++;
                    }
                } else if (else_pc != 0xFFFFu) {
                    next = else_pc;
                    if (sp < 8u) {
                        SK_KIND(sp) = 0u;
                        SK_ELSE(sp) = 0xFFFFFFFFu;
                        SK_END(sp) = end_pc;
                        sp++;
                    }
                } else next = end_pc;
            } else if (op == 3u || op == 4u) {
                uint body = pc + 1u;
                if (cond && init < endn && body < end_pc && sp < 8u) {
                    SK_KIND(sp) = 1u;
                    SK_ELSE(sp) = 0xFFFFFFFFu;
                    SK_END(sp) = end_pc;
                    SK_I(sp) = init;
                    SK_STEP(sp) = step == 0u ? 1u : step;
                    SK_ENDN(sp) = endn;
                    SK_BODY(sp) = body;
                    sp++;
                } else next = end_pc;
            } else if (op == 0u && cond) {
                uint look = sp;
                while (look > 0u && SK_KIND(look - 1u) != 1u) look--;
                if (look > 0u) { next = SK_END(look - 1u); sp = look - 1u; }
            }
            pc = next;
            continue;
        }
        uint s0 = iw.y & 0x3FFFFFu;
        uint in_src = (iw.y >> 22) & 15u;
        uint texu = (iw.y >> 26) & 15u;
        uint bx2 = (iw.y >> 30) & 1u;
        uint s1 = iw.z & 0x3FFFFFu;
        uint s2 = iw.w & 0x3FFFFFu;
        uint kslot = (iw.w >> 22) & 255u;
#if RSX_INTERP_LEGACY
#if RSX_INTERP_FULLIN
        float4 tc8v = input.tc8;
        float4 tc9v = input.tc9;
        float4 face = input.front ? (float4)1 : (float4)(-1);
#else
        float4 tc8v = 0;
        float4 tc9v = 0;
        float4 face = 0;
#endif
        float4 tc[10] = { input.tc0, input.tc1, input.tc2, input.tc3, input.tc4,
                          input.tc5, input.tc6, input.tc7, tc8v, tc9v };
#endif
        float4 a, b, c;
        {
#if RSX_INTERP_LEGACY
            uint spack[3] = { s0, s1, s2 };
            float4 got[3];
#else
            a = 0; b = 0; c = 0;
#endif
            // Bit 30 of words 2/3: the opcode does not read source 1/2 (set by the encoder).
            uint skipsrc = ((iw.z >> 30) & 1u) << 1 | ((iw.w >> 30) & 1u) << 2;
            [unroll] for (int si = 0; si < 3; si++) {
#if RSX_INTERP_LEGACY
                got[si] = 0;
#endif
                [branch] if (((skipsrc >> si) & 1u) != 0u) continue;
#if RSX_INTERP_LEGACY
                uint pk = spack[si];
#else
                uint pk = si == 0 ? s0 : si == 1 ? s1 : s2;
#endif
                uint ty = pk & 3u;
                uint ix = (pk >> 2) & 63u;
                uint sh = (pk >> 8) & 1u;
                uint ng = (pk >> 9) & 1u;
                uint ab = (pk >> 10) & 1u;
                uint pr = (pk >> 11) & 7u;
                uint sw = (pk >> 14) & 255u;
                float4 vv = 0;
#if RSX_INTERP_FLAT && !RSX_INTERP_LEGACY
                bool swz_done = false;
                if (ty == 0u) {
                    uint rb = (min(ix, RSX_INTERP_REGM) * 2u + sh) * 4u;
                    vv = float4(rff[rb + (sw & 3u)], rff[rb + ((sw >> 2) & 3u)],
                                rff[rb + ((sw >> 4) & 3u)], rff[rb + ((sw >> 6) & 3u)]);
                    swz_done = true;
                }
#else
                if (ty == 0u) vv = RSX_IR(sh, min(ix, RSX_INTERP_REGM));
#endif
                else if (ty == 1u) {
                    uint id = in_src;
                    if (id == 0u) vv = wpos;
                    else if (id == 1u) vv = saturate(input.col0);
                    else if (id == 2u) vv = saturate(input.col1);
                    else if (id == 3u) vv = (flags & 32u) != 0u ? rsx_i_fog(input.fog.x) : input.fog;
                    else if (id >= 4u && id <= 13u) vv = tc[id - 4u];
                    else if (id == 14u) vv = face;
                    if (persp != 0u && id >= 4u && id <= 13u) vv *= (1.0 / input.position.w);
                } else vv = fp_constants[kslot];
#if RSX_INTERP_FLAT && !RSX_INTERP_LEGACY
                [branch] if (!swz_done && sw != 228u) vv = rsx_i_swz(vv, sw);
#elif RSX_INTERP_SHORT
                [branch] if (sw != 228u) vv = rsx_i_swz(vv, sw);
#else
                vv = rsx_i_swz(vv, sw);
#endif
#if RSX_INTERP_LEGACY
                got[si] = rsx_i_modify(vv, ab, pr, ng);
#else
#if RSX_INTERP_SHORT
                [branch] if (((pk >> 9) & 0x1Fu) != 0u) vv = rsx_i_modify(vv, ab, pr, ng);
#else
                vv = rsx_i_modify(vv, ab, pr, ng);
#endif
                if (si == 0) a = vv; else if (si == 1) b = vv; else c = vv;
#endif
            }
#if RSX_INTERP_LEGACY
            a = got[0]; b = got[1]; c = got[2];
#endif
        }
        float4 v = 0;
        uint do_alu = 1u;
        if (op == 0x12u) {
            do_alu = 0u;
            bool kill = false;
            if (pred == 0u) kill = true;
            else if (pred == 2u) {
                float4 cv = rsx_i_swz(ccread != 0u ? cc1 : cc0, (iw.z >> 22) & 255u);
                kill = any(rsx_i_cmp4(cv, cmpop) != 0.0);
            }
            if (kill) killed = 1u;
        } else if (op == 0x00u || op == 0x3Du || op == 0x3Eu) do_alu = 0u;
        else if (op == 0x01u) v = a;
        else if (op == 0x02u) v = a * b;
        else if (op == 0x03u) v = a + b;
        else if (op == 0x04u) v = a * b + c;
        else if (op == 0x05u) v = dot(a.xyz, b.xyz).xxxx;
        else if (op == 0x06u) v = dot(a, b).xxxx;
        else if (op == 0x08u) v = min(a, b);
        else if (op == 0x09u) v = max(a, b);
        else if (op == 0x0Au) v = (float4)(a < b);
        else if (op == 0x0Bu) v = (float4)(a >= b);
        else if (op == 0x0Cu) v = (float4)(a <= b);
        else if (op == 0x0Du) v = (float4)(a > b);
        else if (op == 0x0Eu) v = (float4)(a != b);
        else if (op == 0x0Fu) v = (float4)(a == b);
        else if (op == 0x10u) v = frac(a);
        else if (op == 0x11u) v = floor(a);
        else if (op == 0x13u) v = asfloat(rsx_i_pk4b(a)).xxxx;
        else if (op == 0x14u) v = rsx_i_up4b(a.x);
        else if (op == 0x1Au) v = (1.0 / a.x).xxxx;
        else if (op == 0x1Bu) v = rsqrt(abs(a.x)).xxxx;
        else if (op == 0x1Cu) v = exp2(a.x).xxxx;
        else if (op == 0x1Du) v = (lg2abs ? log2(abs(a.x)) : log2(a.x)).xxxx;
        else if (op == 0x1Fu) v = lerp(c, b, a);
        else if (op == 0x22u) v = cos(a.x).xxxx;
        else if (op == 0x23u) v = sin(a.x).xxxx;
        else if (op == 0x24u) v = asfloat(rsx_i_pk2h(a)).xxxx;
        else if (op == 0x25u) v = rsx_i_up2h(a.x);
        else if (op == 0x26u) v = pow(a.x, b.x).xxxx;
        else if (op == 0x27u) v = asfloat(rsx_i_pk4ub(a)).xxxx;
        else if (op == 0x28u) v = rsx_i_up4ub(a.x);
        else if (op == 0x29u) v = asfloat(rsx_i_pk2us(a)).xxxx;
        else if (op == 0x2Au) v = rsx_i_up2us(a.x);
        else if (op == 0x2Cu) v = asfloat(rsx_i_pk4ub(rsx_i_lin2srgb(a))).xxxx;
        else if (op == 0x2Du) v = rsx_i_srgb2lin(rsx_i_up4ub(a.x));
        else if (op == 0x38u) v = dot(a.xy, b.xy).xxxx;
        else if (op == 0x39u) v = float4(normalize(a.xyz), 1.0);
        else if (op == 0x3Au) v = a / b.x;
        else if (op == 0x3Bu) v = abs(a) > 0.0 ? a / sqrt(abs(b.x)) : (float4)0.0;
        else if (op == 0x3Cu) v = float4(1.0, a.y, a.y > 0.0 ? exp2(a.w) : 0.0, 1.0);
        else if (op == 0x17u || op == 0x18u || op == 0x2Fu || op == 0x31u) {
            float4 coord = a;
            uint shbit = (shadow & (1u << texu)) != 0u;
            uint kind = op == 0x31u ? 1u : op == 0x2Fu ? 2u : 0u;
            float lod = b.x;
            uint shp = (shape >> (texu * 2u)) & 3u;
            if (shbit != 0u && (op == 0x17u || op == 0x18u)) {
                float4 sc = op == 0x18u ? coord / coord.w : coord;
                uint dw, dh;
                rsx_i_dim2d(texu, dw, dh);
                float2 p = sc.xy * float2(dw, dh) - 0.5;
                float ref = saturate(sc.z);
                uint func = (shadow >> 16) & 7u;
                uint soft = (shadow >> 19) & 1u;
                float sv = soft != 0u
                    ? 0.25 * (rsx_i_shb(texu, p + float2(-0.5, -0.5), ref, (int)dw, (int)dh, func)
                            + rsx_i_shb(texu, p + float2(0.5, -0.5), ref, (int)dw, (int)dh, func)
                            + rsx_i_shb(texu, p + float2(-0.5, 0.5), ref, (int)dw, (int)dh, func)
                            + rsx_i_shb(texu, p + float2(0.5, 0.5), ref, (int)dw, (int)dh, func))
                    : rsx_i_shb(texu, p, ref, (int)dw, (int)dh, func);
                v = sv.xxxx;
            } else {
                if (op == 0x18u && shp != 1u) coord.xyz /= coord.w;
                uint gd = 0u;
                if ((flags & 512u) != 0u && op == 0x17u && (s0 & 3u) == 1u && in_src == 0u &&
                    ((s0 >> 14) & 255u) == 228u && ((s0 >> 9) & 3u) == 0u && ((s0 >> 11) & 7u) == 0u && persp == 0u) {
                    float inv = texu < 4u ? rsx_texscale[texu].x
                        : ((texu & 1u) != 0u ? rsx_texscale_hi[(texu - 4u) >> 1].z : rsx_texscale_hi[(texu - 4u) >> 1].x);
                    float thr = texu < 4u ? 0.015625 : 0.001953125;
                    if (inv >= thr) { v = float4(0.5, 0.5, 0.5, 0.5); gd = 1u; }
                }
#if RSX_INTERP_PROBE == 1
                v = (float4)0.5; gd = 1u;
#endif
                if (gd == 0u) {
                    coord = rsx_i_unnorm(coord, texu, shp, unnorm);
                    uint sm = texu < 8u ? (samp0 >> (texu * 4u)) & 15u : (samp1 >> ((texu - 8u) * 4u)) & 15u;
                    uint dzu = (texu < 4u && shp == 0u) ? (dzword >> texu) & 1u : 0u;
                    uint mpu = (texu < 4u && shp == 0u && kind == 2u) ? (mipmask >> texu) & 1u : 0u;
                    if (shp == 1u) v = rsx_i_scu(texu, sm, coord.xyz, kind, lod);
                    else if (shp == 2u) v = rsx_i_s3d(texu, sm, coord.xyz, kind, lod);
                    else if (mpu != 0u) v = rsx_i_mip(texu, sm, a.xy, lod, (srgb >> texu) & 1u);
                    else v = rsx_i_s2d(texu, sm, coord.xy, kind, lod);
                    if (dzu != 0u) v = rsx_i_dz(v, (dzword >> 8) & 0xFFFu);
                    else if (mpu == 0u && (srgb & (1u << texu)) != 0u) v = rsx_i_s2l(v);
                    if (bx2 != 0u) v = v * 2.0 - 1.0;
                }
            }
        } else do_alu = 0u;
        if (do_alu != 0u && (nowrite == 0u || setcond != 0u)) {
            if (scale == 1u) v *= 2.0;
            else if (scale == 2u) v *= 4.0;
            else if (scale == 3u) v *= 8.0;
            else if (scale == 5u) v /= 2.0;
            else if (scale == 6u) v /= 4.0;
            else if (scale == 7u) v /= 8.0;
            if (osat != 0u) v = saturate(v);
            uint di = min(dest, RSX_INTERP_REGM);
#if RSX_INTERP_WMASK && !RSX_INTERP_LEGACY
            // Store only the enabled, passing components; read the old value only for a predicated
            // condition-code write (DXC splits the register file into one array per component).
            float4 pm = 1;
            if (pred == 1u) pm = 0;
            else if (pred == 2u) {
                float4 cv = rsx_i_swz(ccread != 0u ? cc1 : cc0, (iw.z >> 22) & 255u);
                pm = rsx_i_cmp4(cv, cmpop);
            }
            float4 cur = 0;
            [branch] if (setcond != 0u && pred != 0u) cur = RSX_IR(dhalf, di);
            if (nowrite == 0u && pred != 1u) {
                uint wm = mask;
                if (pred == 2u) wm &= (pm.x != 0.0 ? 1u : 0u) | (pm.y != 0.0 ? 2u : 0u)
                                    | (pm.z != 0.0 ? 4u : 0u) | (pm.w != 0.0 ? 8u : 0u);
                uint ri = di * 2u + dhalf;
                if ((wm & 1u) != 0u) RSX_IWC(ri, 0u, v.x)
                if ((wm & 2u) != 0u) RSX_IWC(ri, 1u, v.y)
                if ((wm & 4u) != 0u) RSX_IWC(ri, 2u, v.z)
                if ((wm & 8u) != 0u) RSX_IWC(ri, 3u, v.w)
            }
            if (setcond != 0u) {
                float4 srcv = v;
                if (nowrite == 0u && pred == 1u) srcv = cur;
                else if (nowrite == 0u && pred == 2u)
                    srcv = float4(pm.x != 0.0 ? v.x : cur.x, pm.y != 0.0 ? v.y : cur.y,
                                  pm.z != 0.0 ? v.z : cur.z, pm.w != 0.0 ? v.w : cur.w);
                float4 cd = ccwrite != 0u ? cc1 : cc0;
                if ((mask & 1u) != 0u) cd.x = srcv.x;
                if ((mask & 2u) != 0u) cd.y = srcv.y;
                if ((mask & 4u) != 0u) cd.z = srcv.z;
                if ((mask & 8u) != 0u) cd.w = srcv.w;
                if (ccwrite != 0u) cc1 = cd; else cc0 = cd;
            }
#else
            float4 cur = RSX_IR(dhalf, di);
            float4 nxt = cur;
            float4 pm = 1;
            if (pred == 1u) pm = 0;
            else if (pred == 2u) {
                float4 cv = rsx_i_swz(ccread != 0u ? cc1 : cc0, (iw.z >> 22) & 255u);
                pm = rsx_i_cmp4(cv, cmpop);
            }
            if (nowrite == 0u && pred != 1u) {
                if ((mask & 1u) != 0u) nxt.x = (pred == 0u || pm.x != 0.0) ? v.x : cur.x;
                if ((mask & 2u) != 0u) nxt.y = (pred == 0u || pm.y != 0.0) ? v.y : cur.y;
                if ((mask & 4u) != 0u) nxt.z = (pred == 0u || pm.z != 0.0) ? v.z : cur.z;
                if ((mask & 8u) != 0u) nxt.w = (pred == 0u || pm.w != 0.0) ? v.w : cur.w;
                RSX_IW(dhalf, di, nxt)
            }
            if (setcond != 0u) {
                float4 srcv = nowrite != 0u ? v : (pred == 1u ? cur : nxt);
                float4 cd = ccwrite != 0u ? cc1 : cc0;
                if ((mask & 1u) != 0u) cd.x = srcv.x;
                if ((mask & 2u) != 0u) cd.y = srcv.y;
                if ((mask & 4u) != 0u) cd.z = srcv.z;
                if ((mask & 8u) != 0u) cd.w = srcv.w;
                if (ccwrite != 0u) cc1 = cd; else cc0 = cd;
            }
#endif
        }
        pc = next;
    }
#if RSX_INTERP_PROBE < 3 && !RSX_INTERP_NODISC
    if (killed != 0u) discard;
#endif
    float4 o0 = (steps >= RSX_FP_INTERP_STEPS && pc < instr_count) ? (float4)0 : RSX_IR(half, rsx_i_expidx(half, 0u));
    if ((flags & 2048u) != 0u) o0 = (float4)1;   // redirected stencil-mask pass: colour forced to 1 (FP_ONE)
    if ((flags & 16u) != 0u) rsx_i_atest(o0.a);
    if ((flags & 128u) != 0u && (flags & 1024u) == 0u) {
        uint2 dq = (uint2)input.position.xy;
        uint ds = (dq.x * 0x8da6b343u) ^ (dq.y * 0xd8163841u);
        ds ^= ds >> 16; ds *= 0x7feb352du; ds ^= ds >> 15; ds *= 0x846ca68bu; ds ^= ds >> 16;
        o0.rgb += ((float)(ds & 0xFFFFu) + (float)(ds >> 16)) * (1.0 / (65535.0 * 255.0)) - (1.0 / 255.0);
    }
    o0 = rsx_i_nan0(o0);
    if ((flags & 1024u) != 0u) o0.rgb = rsx_i_l2s(o0.rgb);
#if RSX_INTERP_MRT
    RSXInterpOut o;
    o.c0 = o0;
    o.c1 = rsx_i_nan0(o1);
    o.c2 = rsx_i_nan0(o2);
    o.c3 = rsx_i_nan0(o3);
    float4 o1 = RSX_IR(half, rsx_i_expidx(half, 1u));
    float4 o2 = RSX_IR(half, rsx_i_expidx(half, 2u));
    float4 o3 = RSX_IR(half, rsx_i_expidx(half, 3u));
    // Declarations above are out of order; the real assignments follow the fixup in the generator.
    return o;
#else
    return o0;
#endif
}
'''


def fix_mrt(body: str) -> str:
    """MRT outputs must be computed before they are stored."""
    old = """#if RSX_INTERP_MRT
    RSXInterpOut o;
    o.c0 = o0;
    o.c1 = rsx_i_nan0(o1);
    o.c2 = rsx_i_nan0(o2);
    o.c3 = rsx_i_nan0(o3);
    float4 o1 = RSX_IR(half, rsx_i_expidx(half, 1u));
    float4 o2 = RSX_IR(half, rsx_i_expidx(half, 2u));
    float4 o3 = RSX_IR(half, rsx_i_expidx(half, 3u));
    // Declarations above are out of order; the real assignments follow the fixup in the generator.
    return o;
#else
    return o0;
#endif
}
"""
    new = """#if RSX_INTERP_MRT
#if RSX_INTERP_TARGETS > 1
    float4 o1 = RSX_IR(half, rsx_i_expidx(half, 1u));
#endif
#if RSX_INTERP_TARGETS > 2
    float4 o2 = RSX_IR(half, rsx_i_expidx(half, 2u));
#endif
#if RSX_INTERP_TARGETS > 3
    float4 o3 = RSX_IR(half, rsx_i_expidx(half, 3u));
#endif
    if ((flags & 1024u) != 0u) {
#if RSX_INTERP_TARGETS > 1
        o1.rgb = rsx_i_l2s(o1.rgb);
#endif
#if RSX_INTERP_TARGETS > 2
        o2.rgb = rsx_i_l2s(o2.rgb);
#endif
#if RSX_INTERP_TARGETS > 3
        o3.rgb = rsx_i_l2s(o3.rgb);
#endif
    }
    RSXInterpOut o;
    o.c0 = o0;
#if RSX_INTERP_TARGETS > 1
    o.c1 = rsx_i_nan0(o1);
#endif
#if RSX_INTERP_TARGETS > 2
    o.c2 = rsx_i_nan0(o2);
#endif
#if RSX_INTERP_TARGETS > 3
    o.c3 = rsx_i_nan0(o3);
#endif
    return o;
#else
    return o0;
#endif
}
"""
    if old not in body:
        raise SystemExit('mrt block not found')
    return body.replace(old, new, 1)


def tex_first(body: str) -> str:
    """RSX_INTERP_SHORT: test the texture opcodes before the ALU chain (each failed test in the chain is a
    compare and branch per interpreted instruction; texture fetches sat about 40 tests down)."""
    nl = chr(10)
    a = body.index("        if (op == 0x12u) {" + nl)
    t = body.index("        else if (op == 0x17u || op == 0x18u || op == 0x2Fu || op == 0x31u) {" + nl, a)
    e_mark = "        } else do_alu = 0u;" + nl
    e = body.index(e_mark, t) + len(e_mark)
    alu = body[a:t]
    tex = body[t:e - len(e_mark)] + "        }" + nl
    chain = (tex.replace("        else if (op == 0x17u", "        if (op == 0x17u", 1)
             + alu.replace("        if (op == 0x12u) {", "        else if (op == 0x12u) {", 1)
             + "        else do_alu = 0u;" + nl)
    new = ("#if RSX_INTERP_SHORT" + nl
           + "#if RSX_INTERP_SWITCH" + nl + op_switch(chain) + "#else" + nl + chain + "#endif" + nl
           + "#else" + nl + body[a:e] + "#endif" + nl)
    return body[:a] + new + body[e:]


def op_switch(chain: str) -> str:
    """RSX_INTERP_SWITCH: the same opcode chain as one switch, which the compiler can make a jump table."""
    import re
    nl = chr(10)
    out = []
    first = True
    cond = re.compile(r"op == (0x[0-9A-F]+u)")

    def cases(line):
        return " ".join("case %s:" % c for c in cond.findall(line))

    for line in chain.split(nl):
        if first and line.startswith("        if (op == "):
            out.append("        switch (op) {")
            out.append("        " + cases(line) + " {")
            first = False
        elif line == "        }":
            out.append("        } break;")
        elif line.startswith("        else if (op == ") and line.endswith("{"):
            out.append("        " + cases(line) + " {")
        elif line.startswith("        } else if (op == "):
            stmt = line[line.index(")", line.index("op ==")) + 1:].strip()
            out.append("        } break;")
            out.append("        " + cases(line) + " " + stmt + " break;")
        elif line.startswith("        else if (op == "):
            head = line[:line.index(")", line.index("op ==")) + 1]
            stmt = line[len(head):].strip()
            out.append("        " + cases(line) + " " + stmt + " break;")
        elif line == "        else do_alu = 0u;":
            out.append("        default: do_alu = 0u; break;")
            out.append("        }")
        else:
            out.append(line)
    text = nl.join(out)
    if "switch (op)" not in text or "default:" not in text:
        raise SystemExit("op_switch: chain shape changed")
    return text if text.endswith(nl) else text + nl


PROBE7 = """
#if defined(RSX_INTERP_PROBE) && RSX_INTERP_PROBE == 7
#undef main
#if RSX_INTERP_MRT
RSXInterpOut main(float4 position : SV_POSITION) { RSXInterpOut po = (RSXInterpOut)0; return po; }
#else
float4 main(float4 position : SV_POSITION) : SV_TARGET { return (float4)0; }
#endif
#endif
"""


def c_string(text: str) -> str:
    parts = []
    chunk = []
    n = 0
    for ch in text:
        if ch == '\\':
            chunk.append('\\\\')
        elif ch == '"':
            chunk.append('\\"')
        elif ch == '\n':
            chunk.append('\\n')
        elif ch == '\r':
            continue
        else:
            chunk.append(ch)
        n += 1
        if n >= 4000:
            parts.append('"' + ''.join(chunk) + '"')
            chunk = []
            n = 0
    if chunk:
        parts.append('"' + ''.join(chunk) + '"')
    return '\n'.join(parts)


def main():
    body = tex_first(fix_mrt(BODY.replace(chr(13), '')))
    # Drop the unused helper that referenced a dummy dither body.
    text = texture_block() + "\n" + body + PROBE7
    OUT.parent.mkdir(parents=True, exist_ok=True)
    inc = (
        "/* Generated by scripts/test/gen_fp_interp_hlsl.py. Do not edit. */\n"
        "static const char rsx_fp_interp_hlsl[] =\n"
        + c_string(text)
        + ";\n"
    )
    OUT.write_bytes(inc.encode('ascii'))
    (OUT.parent / "rsx_fp_interp_debug.hlsl").write_bytes(text.encode('ascii'))
    print(f"wrote {OUT} ({len(inc)} bytes) hlsl {len(text)}")


if __name__ == '__main__':
    main()
