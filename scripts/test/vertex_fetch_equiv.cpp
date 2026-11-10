// Developer check: do two GPU-fetch vertex shader texts compute the same outputs?
//
//   vertex_fetch_equiv <dirA> <dirB> [--warp] [--trials N] [--vertices N] [--seed S]
//
// Each *.gpu1.vs_5_0.hlsl in dirA is paired with the same file name in dirB (see
// scripts/test/vertex_hlsl_dump.py, which writes game-derived text: keep both folders under scratch/).
// The vertex shader's main() is renamed and called from a compute shader that runs N vertex ids,
// with random constants, random fetch descriptors (every attribute type and component count) and a
// random raw vertex buffer, and writes the 14 float4 outputs. Both texts get identical inputs; the
// outputs are compared bit for bit (all NaNs are equal). The default adapter is the hardware GPU;
// --warp uses the software rasteriser. Exit 0 only if every output of every pair is identical.
//
// Build (x64 VS environment): clang-cl /nologo /O2 /EHsc vertex_fetch_equiv.cpp d3d11.lib d3dcompiler.lib
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static const char* kFields[14] = {"pos", "col0", "col1", "fog", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7", "t8", "t9"};
static const int kThreads = 64;

static std::string wrap(std::string text)
{
    const std::string entry = "VSOutput main(VSInput input)";
    size_t at = text.find(entry);
    if (at == std::string::npos) return {};
    text.replace(at, entry.size(), "VSOutput vs_main(VSInput input)");
    text += "\nRWByteAddressBuffer out_buf : register(u0);\n"
            "cbuffer Cfg : register(b5) { uint cfg_base; uint cfg_count; uint2 cfg_pad; };\n"
            "[numthreads(64, 1, 1)] void main(uint3 id : SV_DispatchThreadID) {\n"
            "    if (id.x >= cfg_count) return;\n"
            "    VSInput inp; inp.vid = cfg_base + id.x;\n"
            "    VSOutput o = vs_main(inp);\n";
    for (int i = 0; i < 14; i++) {
        char line[160];
        snprintf(line, sizeof line, "    out_buf.Store4((id.x * 14u + %du) * 16u, asuint(o.%s));\n", i, kFields[i]);
        text += line;
    }
    text += "}\n";
    return text;
}

struct Gpu {
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
};

static ID3D11Buffer* make_buffer(Gpu& g, UINT bytes, UINT bind, UINT misc, const void* data, D3D11_USAGE usage = D3D11_USAGE_DEFAULT, UINT cpu = 0)
{
    D3D11_BUFFER_DESC d = {};
    d.ByteWidth = bytes; d.Usage = usage; d.BindFlags = bind; d.MiscFlags = misc; d.CPUAccessFlags = cpu;
    D3D11_SUBRESOURCE_DATA init = {data, 0, 0};
    ID3D11Buffer* b = nullptr;
    if (FAILED(g.dev->CreateBuffer(&d, data ? &init : nullptr, &b))) return nullptr;
    return b;
}

struct Inputs {
    std::vector<float> vp_c;          // (512 + 2) float4
    std::vector<uint32_t> fetch;      // 16 uint4 + 16 float4
    std::vector<uint8_t> raw;
};

static Inputs make_inputs(uint32_t seed)
{
    std::mt19937 rng(seed);
    auto rf = [&]() {
        uint32_t r = rng();
        if ((r & 15u) == 0) return 0.0f;
        if ((r & 15u) == 1) return 1.0f;
        if ((r & 15u) == 2) return -1.0f;
        return ((int32_t)(rng() % 20001) - 10000) / 2500.0f;
    };
    Inputs in;
    in.vp_c.resize((512 + 2) * 4);
    for (auto& v : in.vp_c) v = rf();
    in.fetch.resize(16 * 4 + 16 * 4);
    for (int k = 0; k < 16; k++) {
        static const uint32_t strides[] = {4, 8, 12, 16, 20, 24, 32};
        uint32_t ty = rng() % 8, n = 1 + rng() % 4;
        bool enabled = (rng() % 8) != 0;
        in.fetch[k * 4 + 0] = (rng() % 4096) * ((rng() & 1) ? 4u : 1u);
        in.fetch[k * 4 + 1] = strides[rng() % 7];
        in.fetch[k * 4 + 2] = ty | (n << 4) | (enabled ? 0x100u : 0u);
        in.fetch[k * 4 + 3] = 0;
        float def[4] = {rf(), rf(), rf(), rf()};
        std::memcpy(&in.fetch[64 + k * 4], def, sizeof def);
    }
    in.raw.resize(1 << 20);
    for (auto& b : in.raw) b = (uint8_t)rng();
    // Mostly finite halves and floats: clear some exponent bits so NaN/Inf do not dominate every output.
    for (size_t i = 0; i + 1 < in.raw.size(); i += 2)
        if ((in.raw[i] & 0x7C) == 0x7C && (rng() & 7)) in.raw[i] &= 0xBF;
    for (size_t i = 0; i + 3 < in.raw.size(); i += 4)
        if ((in.raw[i] & 0x7F) == 0x7F && (rng() & 3)) in.raw[i] &= 0x3F;
    return in;
}

struct Result {
    bool ok = false;
    std::string error;
    std::vector<uint32_t> bits;
};

static ID3D11ComputeShader* compile(Gpu& g, const std::string& source, std::string& error)
{
    std::string cs = wrap(source);
    if (cs.empty()) { error = "no main(VSInput) entry"; return nullptr; }
    ID3DBlob *code = nullptr, *err = nullptr;
    HRESULT hr = D3DCompile(cs.data(), cs.size(), "equiv", nullptr, nullptr, "main", "cs_5_0", 0, 0, &code, &err);
    if (FAILED(hr)) {
        error = err ? std::string((const char*)err->GetBufferPointer(), err->GetBufferSize()) : "compile failed";
        if (err) err->Release();
        return nullptr;
    }
    if (err) err->Release();
    ID3D11ComputeShader* shader = nullptr;
    if (FAILED(g.dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader))) error = "CreateComputeShader failed";
    code->Release();
    return shader;
}

static Result run(Gpu& g, ID3D11ComputeShader* shader, const Inputs& in, uint32_t count, uint32_t base)
{
    Result r;
    ID3D11Buffer* b0 = make_buffer(g, (UINT)(in.vp_c.size() * 4), D3D11_BIND_CONSTANT_BUFFER, 0, in.vp_c.data());
    ID3D11Buffer* b2 = make_buffer(g, (UINT)(in.fetch.size() * 4), D3D11_BIND_CONSTANT_BUFFER, 0, in.fetch.data());
    uint32_t cfg[4] = {base, count, 0, 0};
    ID3D11Buffer* b5 = make_buffer(g, 16, D3D11_BIND_CONSTANT_BUFFER, 0, cfg);
    ID3D11Buffer* raw = make_buffer(g, (UINT)in.raw.size(), D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, in.raw.data());
    UINT out_bytes = count * 14 * 16;
    ID3D11Buffer* out = make_buffer(g, out_bytes, D3D11_BIND_UNORDERED_ACCESS, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, nullptr);
    ID3D11Buffer* stage = make_buffer(g, out_bytes, 0, 0, nullptr, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ);
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11UnorderedAccessView* uav = nullptr;
    if (b0 && b2 && b5 && raw && out && stage) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = DXGI_FORMAT_R32_TYPELESS; sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        sd.BufferEx.FirstElement = 0; sd.BufferEx.NumElements = (UINT)(in.raw.size() / 4); sd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.Format = DXGI_FORMAT_R32_TYPELESS; ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.FirstElement = 0; ud.Buffer.NumElements = out_bytes / 4; ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        if (SUCCEEDED(g.dev->CreateShaderResourceView(raw, &sd, &srv)) && SUCCEEDED(g.dev->CreateUnorderedAccessView(out, &ud, &uav))) {
            g.ctx->CSSetShader(shader, nullptr, 0);
            ID3D11Buffer* cbs[1] = {b0};
            g.ctx->CSSetConstantBuffers(0, 1, cbs);
            cbs[0] = b2; g.ctx->CSSetConstantBuffers(2, 1, cbs);
            cbs[0] = b5; g.ctx->CSSetConstantBuffers(5, 1, cbs);
            g.ctx->CSSetShaderResources(20, 1, &srv);
            g.ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
            g.ctx->Dispatch((count + kThreads - 1) / kThreads, 1, 1);
            g.ctx->CopyResource(stage, out);
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(g.ctx->Map(stage, 0, D3D11_MAP_READ, 0, &m))) {
                r.bits.assign((const uint32_t*)m.pData, (const uint32_t*)m.pData + out_bytes / 4);
                g.ctx->Unmap(stage, 0);
                r.ok = true;
            } else r.error = "Map failed";
            ID3D11ShaderResourceView* nullsrv = nullptr; ID3D11UnorderedAccessView* nulluav = nullptr;
            g.ctx->CSSetShaderResources(20, 1, &nullsrv);
            g.ctx->CSSetUnorderedAccessViews(0, 1, &nulluav, nullptr);
        } else r.error = "view creation failed";
    } else r.error = "buffer creation failed";
    for (IUnknown* u : {(IUnknown*)srv, (IUnknown*)uav, (IUnknown*)b0, (IUnknown*)b2, (IUnknown*)b5, (IUnknown*)raw, (IUnknown*)out, (IUnknown*)stage, (IUnknown*)shader})
        if (u && u != (IUnknown*)shader) u->Release();
    return r;
}

static bool is_nan_bits(uint32_t b) { return (b & 0x7F800000u) == 0x7F800000u && (b & 0x7FFFFFu); }

int main(int argc, char** argv)
{
    if (argc < 3) { std::fprintf(stderr, "usage: vertex_fetch_equiv <dirA> <dirB> [--warp] [--trials N] [--vertices N] [--seed S]\n"); return 2; }
    fs::path dirA = argv[1], dirB = argv[2];
    bool warp = false; int trials = 3; uint32_t vertices = 1024, seed = 1;
    for (int i = 3; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--warp") warp = true;
        else if (a == "--trials" && i + 1 < argc) trials = atoi(argv[++i]);
        else if (a == "--vertices" && i + 1 < argc) vertices = (uint32_t)atoi(argv[++i]);
        else if (a == "--seed" && i + 1 < argc) seed = (uint32_t)atoi(argv[++i]);
    }
    Gpu g;
    D3D_FEATURE_LEVEL level;
    HRESULT hr = D3D11CreateDevice(nullptr, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                   D3D11_SDK_VERSION, &g.dev, &level, &g.ctx);
    if (FAILED(hr)) { std::fprintf(stderr, "no D3D11 device (0x%08lx)\n", (unsigned long)hr); return 2; }
    IDXGIDevice* dxgi = nullptr; IDXGIAdapter* ad = nullptr; DXGI_ADAPTER_DESC desc = {};
    if (SUCCEEDED(g.dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&ad)) && SUCCEEDED(ad->GetDesc(&desc)))
        std::printf("adapter: %ls\n", desc.Description);
    std::vector<fs::path> files;
    for (auto& e : fs::directory_iterator(dirA))
        if (e.path().string().find(".gpu1.vs_5_0.hlsl") != std::string::npos) files.push_back(e.path().filename());
    std::sort(files.begin(), files.end());
    size_t pairs = 0, identical_text = 0, informative = 0, compared_values = 0, mismatched_values = 0, material_values = 0, failed = 0, differing_shaders = 0;
    double max_abs = 0;
    for (auto& name : files) {
        std::ifstream fa(dirA / name, std::ios::binary), fb(dirB / name, std::ios::binary);
        if (!fb) { std::printf("%s: missing in B\n", name.string().c_str()); failed++; continue; }
        std::string ta((std::istreambuf_iterator<char>(fa)), {}), tb((std::istreambuf_iterator<char>(fb)), {});
        pairs++;
        if (ta == tb) identical_text++;
        size_t shader_mismatch = 0;
        std::string ea, eb;
        ID3D11ComputeShader* sa = compile(g, ta, ea);
        ID3D11ComputeShader* sb = compile(g, tb, eb);
        if (!sa || !sb) {
            std::printf("%s: %s\n", name.string().c_str(), (!sa ? "A: " + ea : "B: " + eb).substr(0, 300).c_str());
            failed++;
            if (sa) sa->Release();
            if (sb) sb->Release();
            continue;
        }
        for (int t = 0; t < trials; t++) {
            Inputs in = make_inputs(seed * 7919u + (uint32_t)t * 104729u + (uint32_t)pairs);
            Result ra = run(g, sa, in, vertices, (uint32_t)t * 5000u), rb = run(g, sb, in, vertices, (uint32_t)t * 5000u);
            if (!ra.ok || !rb.ok) {
                std::printf("%s: %s\n", name.string().c_str(), (!ra.ok ? "A: " + ra.error : "B: " + rb.error).substr(0, 300).c_str());
                failed++; break;
            }
            for (size_t i = 0; i < ra.bits.size(); i++) {
                compared_values++;
                uint32_t x = ra.bits[i], y = rb.bits[i];
                if (x != 0 && x != 0x80000000u && (x & 0x7F800000u) != 0x7F800000u) informative++;
                if (x == y || (is_nan_bits(x) && is_nan_bits(y))) continue;
                mismatched_values++; shader_mismatch++;
                float fx, fy; std::memcpy(&fx, &x, 4); std::memcpy(&fy, &y, 4);
                double d = std::fabs((double)fx - (double)fy);
                if (std::isfinite(d)) {
                    max_abs = d > max_abs ? d : max_abs;
                    if (d > 1e-5 * std::fmax(1.0, std::fmax(std::fabs((double)fx), std::fabs((double)fy)))) material_values++;
                } else material_values++;
            }
        }
        sa->Release(); sb->Release();
        if (shader_mismatch) { differing_shaders++; std::printf("%s: %zu differing values\n", name.string().c_str(), shader_mismatch); }
    }
    std::printf("pairs %zu (identical text %zu), failed %zu, trials %d x %u vertices x 14 float4\n", pairs, identical_text, failed, trials, vertices);
    std::printf("compared %zu values (%zu finite and non-zero): bit-different %zu in %zu shaders, beyond 1e-5 relative %zu, max abs diff %.3g\n",
                compared_values, informative, mismatched_values, differing_shaders, material_values, max_abs);
    return (failed == 0 && mismatched_values == 0 && pairs > 0) ? 0 : 1;
}
