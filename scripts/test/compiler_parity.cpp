// Prologue Phase 231 diagnostic, adopted for GT5: do FXC (shader model 5.0) and DXC (6.0) compute the same results for the game's
// generated shaders, on this GPU?
//
//   compiler_parity <dir> <dxcompiler.dll> [--trials N] [--limit N] [--report file.csv]
//                   [--smooth] [--control L] [--dxc-arg ARG]...
//
// <dir> holds HLSL as the engine queues it (RSX_SHADER_SRC_DUMP: <key>.vs_6_0.hlsl / <key>.ps_6_0.hlsl; the files
// are derived from the game: keep them under scratch/). Each text is compiled with FXC (vs/ps/cs_5_0) and with DXC
// (6.0 with the engine's arguments -HV 2018 -Gis, after the engine's resource-array rewrite), and both run on the
// hardware GPU with identical inputs:
//  - vertex shaders: main() is called from a compute shader over 1,024 vertex ids with random constants, random
//    fetch descriptors (every attribute type) and a random vertex buffer; the 14 float4 outputs are compared
//    (vertex shaders without a vertex id input, the CPU-fetch path, are counted as not run);
//  - pixel shaders: a full-screen triangle at 64x64 into one float32 target per SV_TARGETn (and a float depth
//    target for SV_Depth), with random per-vertex varyings (varied w), random 2D / cube / 3D textures matching the
//    shader's declarations (odd sampler slots point-filtered), random constants in every cbuffer. Pixels a shader
//    discards keep a marker, so a discard that differs counts as a coverage difference.
// Values are compared bit for bit (NaNs equal); differing values are classed as NaN on one side only, infinite,
// beyond 1e-5 and beyond 1/1024 of the larger magnitude (at least 1).
//  --smooth      gradient textures and linear filtering everywhere (texel choice less sensitive to coordinates);
//  --control L   compare FXC with FXC at optimisation level L (0..3, 4 = skip optimisation) instead of DXC: the
//                size of differences a legitimate change of arithmetic order produces, as a baseline;
//  --dxc-arg A   add a DXC argument (experiments).
// Exit 0 when every shader ran and nothing was flagged.
//
// Build (x64 VS environment):
//   cl /nologo /std:c++17 /O2 /EHsc compiler_parity.cpp d3d12.lib dxgi.lib d3dcompiler.lib
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <dxcapi.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <regex>
#include <string>
#include <vector>

namespace fs = std::filesystem;
static const int W = 64, H = 64;
static const char* kFields[14] = {"pos", "col0", "col1", "fog", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7", "t8", "t9"};

// ---- the engine's DXC rewrite (rsx_d3d12_backend.c shader_dxc_scalarize), same rules ----
static std::string scalarize(const std::string& src)
{
    struct A { const char* name; char reg; };
    static const A arr[2] = {{"rsx_samp", 's'}, {"rsx_tex", 't'}};
    std::string out = src;
    for (const A& a : arr) {
        std::string pat = std::string(a.name) + "[16] : register(" + a.reg + "0);";
        size_t p = out.find(pat);
        if (p == std::string::npos) continue;
        size_t ls = out.rfind('\n', p); ls = ls == std::string::npos ? 0 : ls + 1;
        std::string type = out.substr(ls, out.find(' ', ls) - ls);
        std::string decl;
        for (int k = 0; k < 16; k++) decl += type + " " + a.name + "_" + std::to_string(k) + " : register(" + a.reg + std::to_string(k) + ");" + (k == 15 ? "" : "\n");
        out.replace(ls, p + pat.size() - ls, decl);
        std::string name = a.name;
        std::string res; res.reserve(out.size());
        for (size_t i = 0; i < out.size();) {
            bool word_start = i == 0 || !(isalnum((unsigned char)out[i - 1]) || out[i - 1] == '_');
            if (word_start && out.compare(i, name.size(), name) == 0 && i + name.size() < out.size() && out[i + name.size()] == '[') {
                size_t j = i + name.size() + 1, d0 = j;
                while (j < out.size() && isdigit((unsigned char)out[j])) j++;
                if (j > d0 && j < out.size() && out[j] == ']' && j - d0 <= 2) { res += name + "_" + out.substr(d0, j - d0); i = j + 1; continue; }
                return src;   // not a literal index: the engine compiles the text as written
            }
            res += out[i++];
        }
        out = res;
    }
    return out;
}

// ---- compilers ----
static IDxcCompiler3* g_dxc;
static std::vector<std::wstring> g_dxc_extra;   // --dxc-arg (repeatable)
static int g_control = -1;   // --control L: compare FXC with FXC at optimisation level L (0..3; 4 = skip optimisation) instead of DXC, as a baseline
static bool g_smooth;   // --smooth: gradient textures and linear filtering (texel choice insensitive to last-bit coordinates)
static std::vector<uint8_t> fxc(const std::string& text, const char* target, std::string& err, UINT flags = 0)
{
    ID3DBlob *code = nullptr, *e = nullptr;
    HRESULT hr = D3DCompile(text.data(), text.size(), "parity", nullptr, nullptr, "main", target, flags, 0, &code, &e);
    std::vector<uint8_t> out;
    if (SUCCEEDED(hr) && code) out.assign((uint8_t*)code->GetBufferPointer(), (uint8_t*)code->GetBufferPointer() + code->GetBufferSize());
    else err = e ? std::string((const char*)e->GetBufferPointer(), e->GetBufferSize()) : "FXC failed";
    if (code) code->Release(); if (e) e->Release();
    return out;
}
static std::vector<uint8_t> dxc(const std::string& text, const wchar_t* target, std::string& err)
{
    if (g_control >= 0) {   // baseline: FXC at another optimisation level, same shader model as the "A" side
        static const UINT level[5] = {D3DCOMPILE_OPTIMIZATION_LEVEL0, D3DCOMPILE_OPTIMIZATION_LEVEL1, D3DCOMPILE_OPTIMIZATION_LEVEL2, D3DCOMPILE_OPTIMIZATION_LEVEL3, D3DCOMPILE_SKIP_OPTIMIZATION};
        char t[8] = {(char)target[0], (char)target[1], '_', '5', '_', '0', 0};
        return fxc(text, t, err, level[g_control > 4 ? 4 : g_control]);   // 4 = skip optimisation
    }
    DxcBuffer buf{text.data(), text.size(), DXC_CP_UTF8};
    std::vector<LPCWSTR> args = {L"-E", L"main", L"-T", target, L"-HV", L"2018", L"-Gis"};   // the engine's arguments
    for (auto& a : g_dxc_extra) args.push_back(a.c_str());
    IDxcResult* r = nullptr;
    std::vector<uint8_t> out;
    if (FAILED(g_dxc->Compile(&buf, args.data(), (UINT32)args.size(), nullptr, IID_PPV_ARGS(&r))) || !r) { err = "DXC call failed"; return out; }
    HRESULT st = E_FAIL; r->GetStatus(&st);
    if (SUCCEEDED(st)) {
        IDxcBlob* o = nullptr; r->GetResult(&o);
        if (o) { out.assign((uint8_t*)o->GetBufferPointer(), (uint8_t*)o->GetBufferPointer() + o->GetBufferSize()); o->Release(); }
    } else {
        IDxcBlobEncoding* e = nullptr; r->GetErrorBuffer(&e);
        err = e ? std::string((const char*)e->GetBufferPointer(), e->GetBufferSize()) : "DXC failed";
        if (e) e->Release();
    }
    r->Release();
    return out;
}

// ---- D3D12 plumbing ----
struct Gpu {
    ID3D12Device* dev = nullptr; ID3D12CommandQueue* q = nullptr; ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr; ID3D12Fence* fence = nullptr; UINT64 fv = 0; HANDLE ev = nullptr;
    void wait() { q->Signal(fence, ++fv); if (fence->GetCompletedValue() < fv) { fence->SetEventOnCompletion(fv, ev); WaitForSingleObject(ev, INFINITE); } }
    void submit() { list->Close(); ID3D12CommandList* l[] = {list}; q->ExecuteCommandLists(1, l); wait(); alloc->Reset(); list->Reset(alloc, nullptr); }
};
static Gpu g;
static std::vector<IUnknown*> g_garbage;
template <class T> static T* keep(T* p) { if (p) g_garbage.push_back(p); return p; }
static void collect() { for (auto* p : g_garbage) p->Release(); g_garbage.clear(); }

static ID3D12Resource* buffer(UINT64 bytes, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES st, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
{
    D3D12_HEAP_PROPERTIES hp = {heap};
    D3D12_RESOURCE_DESC d = {}; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = bytes; d.Height = 1; d.DepthOrArraySize = 1;
    d.MipLevels = 1; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; d.Flags = flags;
    ID3D12Resource* r = nullptr;
    g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, st, nullptr, IID_PPV_ARGS(&r));
    return keep(r);
}
static ID3D12Resource* upload(const void* data, size_t bytes)
{
    ID3D12Resource* r = buffer((bytes + 255) & ~size_t(255), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    void* p = nullptr; r->Map(0, nullptr, &p); std::memcpy(p, data, bytes); r->Unmap(0, nullptr);
    return r;
}
static void barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER br = {}; br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; br.Transition.pResource = r;
    br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; br.Transition.StateBefore = a; br.Transition.StateAfter = b;
    g.list->ResourceBarrier(1, &br);
}
static ID3D12RootSignature* root_signature(const D3D12_ROOT_SIGNATURE_DESC& desc)
{
    ID3DBlob *b = nullptr, *e = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &b, &e))) {
        std::fprintf(stderr, "root signature: %s\n", e ? (const char*)e->GetBufferPointer() : "?"); exit(2);
    }
    ID3D12RootSignature* rs = nullptr;
    g.dev->CreateRootSignature(0, b->GetBufferPointer(), b->GetBufferSize(), IID_PPV_ARGS(&rs));
    b->Release(); if (e) e->Release();
    return rs;
}

// ---- vertex shaders: compute wrapper (as scripts/test/vertex_fetch_equiv.cpp) ----
static ID3D12RootSignature* g_cs_rs;
static std::string wrap_vs(std::string text)
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
    return text + "}\n";
}
struct VsInputs { std::vector<float> vp_c; std::vector<uint32_t> fetch; std::vector<uint8_t> raw; };
static VsInputs vs_inputs(uint32_t seed)
{
    std::mt19937 rng(seed);
    auto rf = [&]() { uint32_t r = rng(); if ((r & 15u) == 0) return 0.0f; if ((r & 15u) == 1) return 1.0f; if ((r & 15u) == 2) return -1.0f;
                      return ((int32_t)(rng() % 20001) - 10000) / 2500.0f; };
    VsInputs in;
    in.vp_c.resize((512 + 2) * 4); for (auto& v : in.vp_c) v = rf();
    in.fetch.resize(128);
    for (int k = 0; k < 16; k++) {
        static const uint32_t strides[] = {4, 8, 12, 16, 20, 24, 32};
        uint32_t ty = rng() % 8, n = 1 + rng() % 4; bool on = (rng() % 8) != 0;
        in.fetch[k * 4 + 0] = (rng() % 4096) * ((rng() & 1) ? 4u : 1u); in.fetch[k * 4 + 1] = strides[rng() % 7];
        in.fetch[k * 4 + 2] = ty | (n << 4) | (on ? 0x100u : 0u); in.fetch[k * 4 + 3] = 0;
        float def[4] = {rf(), rf(), rf(), rf()}; std::memcpy(&in.fetch[64 + k * 4], def, sizeof def);
    }
    in.raw.resize(1 << 20); for (auto& b : in.raw) b = (uint8_t)rng();
    for (size_t i = 0; i + 1 < in.raw.size(); i += 2) if ((in.raw[i] & 0x7C) == 0x7C && (rng() & 7)) in.raw[i] &= 0xBF;
    for (size_t i = 0; i + 3 < in.raw.size(); i += 4) if ((in.raw[i] & 0x7F) == 0x7F && (rng() & 3)) in.raw[i] &= 0x3F;
    return in;
}
static bool run_vs(const std::vector<uint8_t>& code, const VsInputs& in, std::vector<uint32_t>& bits, std::string& err)
{
    const uint32_t count = 1024;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {}; pd.pRootSignature = g_cs_rs; pd.CS = {code.data(), code.size()};
    ID3D12PipelineState* pso = nullptr;
    if (FAILED(g.dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)))) { err = "CreateComputePipelineState failed"; return false; }
    keep(pso);
    uint32_t cfg[4] = {7, count, 0, 0};
    ID3D12Resource *b0 = upload(in.vp_c.data(), in.vp_c.size() * 4), *b2 = upload(in.fetch.data(), in.fetch.size() * 4), *b5 = upload(cfg, 16);
    ID3D12Resource* raw = upload(in.raw.data(), in.raw.size());
    UINT64 bytes = count * 14 * 16;
    ID3D12Resource* out = buffer(bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    ID3D12Resource* rb = buffer(bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    g.list->SetComputeRootSignature(g_cs_rs);
    g.list->SetPipelineState(pso);
    g.list->SetComputeRootConstantBufferView(0, b0->GetGPUVirtualAddress());
    g.list->SetComputeRootConstantBufferView(1, b2->GetGPUVirtualAddress());
    g.list->SetComputeRootConstantBufferView(2, b5->GetGPUVirtualAddress());
    g.list->SetComputeRootShaderResourceView(3, raw->GetGPUVirtualAddress());
    g.list->SetComputeRootUnorderedAccessView(4, out->GetGPUVirtualAddress());
    g.list->Dispatch((count + 63) / 64, 1, 1);
    barrier(out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    g.list->CopyResource(rb, out);
    g.submit();
    void* p = nullptr; rb->Map(0, nullptr, &p);
    bits.assign((uint32_t*)p, (uint32_t*)p + bytes / 4); rb->Unmap(0, nullptr);
    return true;
}

// ---- pixel shaders: full-screen draw ----
static ID3D12RootSignature* g_ps_rs;
static ID3D12DescriptorHeap *g_srv_heap, *g_rtv_heap, *g_dsv_heap;
static ID3D12Resource *g_tex2d, *g_texcube, *g_tex3d;
static UINT g_srv_inc, g_rtv_inc;
static const char* kHarnessVs =
    "cbuffer V : register(b13) { float4 v[3 * 14]; };\n"
    "struct O { float4 pos:SV_Position; float4 col0:COLOR0; float4 col1:COLOR1; float4 fog:FOG;\n"
    "  float4 t0:TEXCOORD0; float4 t1:TEXCOORD1; float4 t2:TEXCOORD2; float4 t3:TEXCOORD3; float4 t4:TEXCOORD4;\n"
    "  float4 t5:TEXCOORD5; float4 t6:TEXCOORD6; float4 t7:TEXCOORD7; float4 t8:TEXCOORD8; float4 t9:TEXCOORD9; };\n"
    "O main(uint id : SV_VertexID) { O o; uint b = id * 14;\n"
    "  float2 xy = float2(id == 1 ? 3.0 : -1.0, id == 2 ? 3.0 : -1.0); float w = v[b].w;\n"
    "  o.pos = float4(xy * w, v[b].z * w, w); o.col0 = v[b+1]; o.col1 = v[b+2]; o.fog = v[b+3];\n"
    "  o.t0 = v[b+4]; o.t1 = v[b+5]; o.t2 = v[b+6]; o.t3 = v[b+7]; o.t4 = v[b+8]; o.t5 = v[b+9]; o.t6 = v[b+10];\n"
    "  o.t7 = v[b+11]; o.t8 = v[b+12]; o.t9 = v[b+13]; return o; }\n";
static std::vector<uint8_t> g_vs_fxc, g_vs_dxc;

static ID3D12Resource* texture(D3D12_RESOURCE_DIMENSION dim, UINT w, UINT h, UINT depth, std::mt19937& rng)
{
    D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC d = {}; d.Dimension = dim; d.Width = w; d.Height = h; d.DepthOrArraySize = (UINT16)depth; d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
    ID3D12Resource* t = nullptr;
    g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&t));
    UINT subs = dim == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : depth;
    UINT64 total = 0; std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fp(subs); std::vector<UINT> rows(subs); std::vector<UINT64> rowb(subs);
    g.dev->GetCopyableFootprints(&d, 0, subs, 0, fp.data(), rows.data(), rowb.data(), &total);
    std::vector<uint8_t> data(total); for (auto& b : data) b = (uint8_t)rng();
    if (g_smooth) for (UINT64 i = 0; i + 3 < total; i += 4) {   // slow gradients (the copy footprint is close enough to row order)
        UINT64 px = i / 4; data[i] = (uint8_t)(px % 64 * 4); data[i + 1] = (uint8_t)(px / 64 % 64 * 4);
        data[i + 2] = (uint8_t)((px % 64 + px / 64 % 64) * 2); data[i + 3] = 255; }
    ID3D12Resource* up = upload(data.data(), data.size());
    for (UINT s = 0; s < subs; s++) {
        D3D12_TEXTURE_COPY_LOCATION dst = {t, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX}; dst.SubresourceIndex = s;
        D3D12_TEXTURE_COPY_LOCATION src = {up, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT}; src.PlacedFootprint = fp[s];
        g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    barrier(t, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    g.submit();
    return t;   // kept for the whole run
}

struct PsShape { int targets = 1; bool depth = false; char kind[16]; std::vector<int> cbuffers; };
static PsShape ps_shape(const std::string& text)
{
    PsShape s; std::memset(s.kind, '2', sizeof s.kind);
    int maxt = -1;
    std::smatch m;
    std::regex target("SV_TARGET([0-9])", std::regex::icase);
    for (auto it = std::sregex_iterator(text.begin(), text.end(), target); it != std::sregex_iterator(); ++it) maxt = std::max(maxt, std::stoi((*it)[1]));
    s.targets = maxt < 0 ? 1 : maxt + 1;
    s.depth = std::regex_search(text, std::regex("SV_Depth", std::regex::icase));
    std::regex decl("(TextureCube|Texture3D|Texture2D)\\s+rsx_tex([0-9]+)\\s*:\\s*register\\(t([0-9]+)\\)");
    for (auto it = std::sregex_iterator(text.begin(), text.end(), decl); it != std::sregex_iterator(); ++it) {
        int reg = std::stoi((*it)[3]); if (reg < 16) s.kind[reg] = (*it)[1] == "TextureCube" ? 'c' : (*it)[1] == "Texture3D" ? '3' : '2';
    }
    std::regex cb("register\\(b([0-9]+)\\)");
    for (auto it = std::sregex_iterator(text.begin(), text.end(), cb); it != std::sregex_iterator(); ++it) s.cbuffers.push_back(std::stoi((*it)[1]));
    return s;
}
static bool run_ps(const std::vector<uint8_t>& vs, const std::vector<uint8_t>& ps, const PsShape& shape, uint32_t seed,
                   std::vector<uint32_t>& bits, std::string& err)
{
    std::mt19937 rng(seed);
    auto rf = [&](float lo, float hi) { return lo + (hi - lo) * (float)(rng() % 1000001) / 1000000.0f; };
    // SRVs for this shader's declared texture kinds.
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_srv_heap->GetCPUDescriptorHandleForHeapStart();
    for (int i = 0; i < 16; i++, h.ptr += g_srv_inc) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {}; sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        ID3D12Resource* r = g_tex2d;
        if (shape.kind[i] == 'c') { sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE; sd.TextureCube.MipLevels = 1; r = g_texcube; }
        else if (shape.kind[i] == '3') { sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D; sd.Texture3D.MipLevels = 1; r = g_tex3d; }
        else { sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1; }
        g.dev->CreateShaderResourceView(r, &sd, h);
    }
    // Pipeline.
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = g_ps_rs; pd.VS = {vs.data(), vs.size()}; pd.PS = {ps.data(), ps.size()};
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = 0xF;
    for (int i = 1; i < 8; i++) pd.BlendState.RenderTarget[i].RenderTargetWriteMask = 0xF;
    pd.SampleMask = 0xFFFFFFFF; pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = FALSE;
    if (shape.depth) { pd.DepthStencilState.DepthEnable = TRUE; pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
                       pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS; pd.DSVFormat = DXGI_FORMAT_D32_FLOAT; }
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = (UINT)shape.targets;
    for (int i = 0; i < shape.targets; i++) pd.RTVFormats[i] = DXGI_FORMAT_R32G32B32A32_FLOAT;
    pd.SampleDesc.Count = 1;
    ID3D12PipelineState* pso = nullptr;
    HRESULT hr = g.dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso));
    if (FAILED(hr)) { char b[64]; snprintf(b, sizeof b, "CreateGraphicsPipelineState 0x%08lx", (unsigned long)hr); err = b; return false; }
    keep(pso);
    // Targets (cleared to a marker a shader never writes by chance).
    const float marker[4] = {-12345.0f, -12345.0f, -12345.0f, -12345.0f};
    std::vector<ID3D12Resource*> rts;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtv_heap->GetCPUDescriptorHandleForHeapStart(), first = rtv;
    for (int i = 0; i < shape.targets; i++, rtv.ptr += g_rtv_inc) {
        D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC d = {}; d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = W; d.Height = H; d.DepthOrArraySize = 1;
        d.MipLevels = 1; d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; d.SampleDesc.Count = 1; d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE cv = {DXGI_FORMAT_R32G32B32A32_FLOAT}; std::memcpy(cv.Color, marker, 16);
        ID3D12Resource* t = nullptr;
        g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&t));
        keep(t); rts.push_back(t);
        g.dev->CreateRenderTargetView(t, nullptr, rtv);
        g.list->ClearRenderTargetView(rtv, marker, 0, nullptr);
    }
    ID3D12Resource* depth = nullptr; D3D12_CPU_DESCRIPTOR_HANDLE dsv = g_dsv_heap->GetCPUDescriptorHandleForHeapStart();
    if (shape.depth) {
        D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC d = {}; d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = W; d.Height = H; d.DepthOrArraySize = 1;
        d.MipLevels = 1; d.Format = DXGI_FORMAT_D32_FLOAT; d.SampleDesc.Count = 1; d.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE cv = {DXGI_FORMAT_D32_FLOAT}; cv.DepthStencil.Depth = 0.123f;
        g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, IID_PPV_ARGS(&depth));
        keep(depth);
        g.dev->CreateDepthStencilView(depth, nullptr, dsv);
        g.list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.123f, 0, 0, nullptr);
    }
    // Inputs: per-vertex varyings (w 0.5..2), constants for every cbuffer slot the shader declares.
    std::vector<float> verts(3 * 14 * 4);
    for (int vtx = 0; vtx < 3; vtx++) {
        float* p = &verts[vtx * 56];
        p[0] = 0; p[1] = 0; p[2] = rf(0.0f, 1.0f); p[3] = rf(0.5f, 2.0f);
        for (int i = 4; i < 56; i++) p[i] = (i % 8 < 4) ? rf(0.0f, 1.0f) : rf(-2.0f, 2.0f);
    }
    ID3D12Resource* vb = upload(verts.data(), verts.size() * 4);
    std::vector<float> consts(16384 * 4);   // 256 KB: eight root CBVs 4 KB apart, each may read 64 KB
    for (auto& c : consts) { uint32_t r = rng() % 16; c = r == 0 ? 0.0f : r == 1 ? 1.0f : r == 2 ? 0.5f : rf(-2.0f, 2.0f); }
    ID3D12Resource* cb = upload(consts.data(), consts.size() * 4);
    g.list->SetGraphicsRootSignature(g_ps_rs);
    ID3D12DescriptorHeap* heaps[] = {g_srv_heap}; g.list->SetDescriptorHeaps(1, heaps);
    g.list->SetGraphicsRootDescriptorTable(0, g_srv_heap->GetGPUDescriptorHandleForHeapStart());
    for (int b = 0; b < 8; b++) g.list->SetGraphicsRootConstantBufferView(1 + b, cb->GetGPUVirtualAddress() + (UINT64)b * 4096);
    g.list->SetGraphicsRootConstantBufferView(9, vb->GetGPUVirtualAddress());
    g.list->SetPipelineState(pso);
    g.list->OMSetRenderTargets((UINT)shape.targets, &first, TRUE, shape.depth ? &dsv : nullptr);
    D3D12_VIEWPORT vp = {0, 0, (float)W, (float)H, 0, 1}; D3D12_RECT sc = {0, 0, W, H};
    g.list->RSSetViewports(1, &vp); g.list->RSSetScissorRects(1, &sc);
    g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.list->DrawInstanced(3, 1, 0, 0);
    // Read back every target (row pitch 1024 bytes: already 256-aligned).
    std::vector<ID3D12Resource*> all = rts; if (depth) all.push_back(depth);
    std::vector<ID3D12Resource*> rbs;
    for (size_t i = 0; i < all.size(); i++) {
        bool d = all[i] == depth; UINT bpp = d ? 4 : 16;
        barrier(all[i], d ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        ID3D12Resource* rb = buffer((UINT64)((W * bpp + 255) & ~255) * H, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        rbs.push_back(rb);
        D3D12_TEXTURE_COPY_LOCATION src = {all[i], D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        D3D12_TEXTURE_COPY_LOCATION dst = {rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        dst.PlacedFootprint.Footprint = {d ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R32G32B32A32_FLOAT, (UINT)W, (UINT)H, 1, (UINT)((W * bpp + 255) & ~255)};
        g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    g.submit();
    bits.clear();
    for (size_t i = 0; i < rbs.size(); i++) {
        bool d = all[i] == depth; UINT bpp = d ? 4 : 16, pitch = (W * bpp + 255) & ~255;
        uint8_t* p = nullptr; rbs[i]->Map(0, nullptr, (void**)&p);
        for (int y = 0; y < H; y++) bits.insert(bits.end(), (uint32_t*)(p + y * pitch), (uint32_t*)(p + y * pitch + W * bpp));
        rbs[i]->Unmap(0, nullptr);
    }
    return true;
}

// ---- comparison ----
struct Stats { size_t values = 0, bit_diff = 0, rel5 = 0, abs1024 = 0, coverage = 0, nan_fxc = 0, nan_dxc = 0, inf = 0; double max_abs = 0, max_rel = 0; };
static bool nan_bits(uint32_t b) { return (b & 0x7F800000u) == 0x7F800000u && (b & 0x7FFFFFu); }
static void compare(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b, Stats& s)
{
    const uint32_t mark = 0xC640E400u;   // -12345.0f
    for (size_t i = 0; i < a.size() && i < b.size(); i++) {
        s.values++;
        uint32_t x = a[i], y = b[i];
        if (x == y || (nan_bits(x) && nan_bits(y))) continue;
        s.bit_diff++;
        if ((x == mark) != (y == mark)) { s.coverage++; continue; }
        float fx, fy; std::memcpy(&fx, &x, 4); std::memcpy(&fy, &y, 4);
        double d = std::fabs((double)fx - (double)fy);
        if (nan_bits(x) != nan_bits(y)) { (nan_bits(x) ? s.nan_fxc : s.nan_dxc)++; continue; }
        if (!std::isfinite(fx) || !std::isfinite(fy)) { s.inf++; continue; }
        // Relative to the larger magnitude (at least 1): random vertex inputs reach 1e31.
        double scale = std::max(1.0, std::max(std::fabs((double)fx), std::fabs((double)fy)));
        s.max_abs = std::max(s.max_abs, d); s.max_rel = std::max(s.max_rel, d / scale);
        if (d > 1e-5 * scale) s.rel5++;
        if (d > scale / 1024) s.abs1024++;
    }
}

int main(int argc, char** argv)
{
    if (argc < 3) { std::fprintf(stderr, "usage: compiler_parity <dir> <dxcompiler.dll> [--trials N] [--limit N] [--report file.csv]\n"); return 2; }
    fs::path dir = argv[1]; int trials = 2; size_t limit = SIZE_MAX; std::string report;
    for (int i = 3; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--trials" && i + 1 < argc) trials = atoi(argv[++i]);
        else if (a == "--limit" && i + 1 < argc) limit = (size_t)atoll(argv[++i]);
        else if (a == "--report" && i + 1 < argc) report = argv[++i];
        else if (a == "--smooth") g_smooth = true;
        else if (a == "--control" && i + 1 < argc) g_control = atoi(argv[++i]);
        else if (a == "--dxc-arg" && i + 1 < argc) { std::string v = argv[++i]; g_dxc_extra.emplace_back(v.begin(), v.end()); }
    }
    if (trials < 1 || trials > 32 || !limit) { std::fprintf(stderr, "trials must be 1..32 and limit must be positive\n"); return 2; }
    HMODULE m = LoadLibraryExA(argv[2], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    auto create = m ? (DxcCreateInstanceProc)GetProcAddress(m, "DxcCreateInstance") : nullptr;
    if (!create || FAILED(create(CLSID_DxcCompiler, IID_PPV_ARGS(&g_dxc)))) { std::fprintf(stderr, "cannot load %s\n", argv[2]); return 2; }
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g.dev)))) { std::fprintf(stderr, "no D3D12 device\n"); return 2; }
    { IDXGIFactory4* f = nullptr; CreateDXGIFactory1(IID_PPV_ARGS(&f)); IDXGIAdapter1* ad = nullptr; DXGI_ADAPTER_DESC1 d = {};
      if (f && SUCCEEDED(f->EnumAdapterByLuid(g.dev->GetAdapterLuid(), IID_PPV_ARGS(&ad))) && SUCCEEDED(ad->GetDesc1(&d))) std::printf("adapter: %ls\n", d.Description);
      if (ad) ad->Release(); if (f) f->Release(); }
    D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT};
    g.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.q));
    g.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.alloc));
    g.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc, nullptr, IID_PPV_ARGS(&g.list));
    g.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence)); g.ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    // Root signatures: compute (b0, b2, b5, t20, u0); graphics (t0..t15 table, b0..b7, b13, static s0..s15).
    D3D12_ROOT_PARAMETER cp[5] = {};
    UINT cregs[3] = {0, 2, 5};
    for (int i = 0; i < 3; i++) { cp[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; cp[i].Descriptor.ShaderRegister = cregs[i]; }
    cp[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; cp[3].Descriptor.ShaderRegister = 20;
    cp[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; cp[4].Descriptor.ShaderRegister = 0;
    D3D12_ROOT_SIGNATURE_DESC crs = {5, cp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    g_cs_rs = root_signature(crs);
    D3D12_DESCRIPTOR_RANGE range = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 16, 0, 0, 0};
    D3D12_ROOT_PARAMETER gp[10] = {};
    gp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; gp[0].DescriptorTable = {1, &range};
    for (int b = 0; b < 8; b++) { gp[1 + b].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; gp[1 + b].Descriptor.ShaderRegister = b; }
    gp[9].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; gp[9].Descriptor.ShaderRegister = 13; gp[9].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    D3D12_STATIC_SAMPLER_DESC ss[16] = {};
    for (int i = 0; i < 16; i++) {
        ss[i].Filter = (i & 1) && !g_smooth ? D3D12_FILTER_MIN_MAG_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        ss[i].AddressU = ss[i].AddressV = ss[i].AddressW = (i & 2) ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        ss[i].MaxLOD = D3D12_FLOAT32_MAX; ss[i].ShaderRegister = i; ss[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        ss[i].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    }
    D3D12_ROOT_SIGNATURE_DESC grs = {10, gp, 16, ss, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    g_ps_rs = root_signature(grs);
    D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 16, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    g.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_srv_heap));
    hd = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 8}; g.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_rtv_heap));
    hd = {D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1}; g.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_dsv_heap));
    g_srv_inc = g.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_rtv_inc = g.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    std::mt19937 trng(12345);
    g_tex2d = texture(D3D12_RESOURCE_DIMENSION_TEXTURE2D, 64, 64, 1, trng);
    g_texcube = texture(D3D12_RESOURCE_DIMENSION_TEXTURE2D, 32, 32, 6, trng);
    g_tex3d = texture(D3D12_RESOURCE_DIMENSION_TEXTURE3D, 16, 16, 8, trng);
    g_garbage.clear();   // the textures stay
    std::string e1, e2;
    g_vs_fxc = fxc(kHarnessVs, "vs_5_0", e1); g_vs_dxc = dxc(kHarnessVs, L"vs_6_0", e2);
    if (g_vs_fxc.empty() || g_vs_dxc.empty()) { std::fprintf(stderr, "harness VS: %s %s\n", e1.c_str(), e2.c_str()); return 2; }

    std::vector<fs::path> files;
    for (auto& e : fs::directory_iterator(dir)) if (e.path().extension() == ".hlsl") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    if (files.empty()) { std::fprintf(stderr, "no HLSL sources found\n"); return 2; }
    if (files.size() > limit) files.resize(limit);
    FILE* csv = report.empty() ? nullptr : std::fopen(report.c_str(), "w");
    if (csv) std::fprintf(csv, "file,kind,values,bit_diff,rel5,rel1024,coverage,nan_fxc,nan_dxc,inf,max_abs,max_rel,error\n");
    Stats total[2]; size_t shaders[2] = {0, 0}, failed = 0, unsupported = 0, differing[2] = {0, 0}, visible[2] = {0, 0}, scalarized = 0;
    for (auto& path : files) {
        std::ifstream in(path, std::ios::binary); std::string text((std::istreambuf_iterator<char>(in)), {});
        std::string name = path.filename().string();
        bool vs = name.find(".vs_") != std::string::npos;
        int k = vs ? 0 : 1;
        std::string ef, ed, err;
        std::vector<uint8_t> cf, cd;
        if (vs) {
            std::string cs = wrap_vs(text);
            if (cs.empty()) { err = "no VSOutput main(VSInput input)"; }
            else { cf = fxc(cs, "cs_5_0", ef); cd = dxc(cs, L"cs_6_0", ed); }
        } else {
            std::string s = scalarize(text); if (s != text) scalarized++;
            cf = fxc(text, "ps_5_0", ef); cd = dxc(s, L"ps_6_0", ed);
        }
        if (err.empty() && (cf.empty() || cd.empty())) err = (cf.empty() ? "FXC: " + ef : "DXC: " + ed).substr(0, 200);
        Stats s;
        PsShape shape = vs ? PsShape() : ps_shape(text);
        for (int t = 0; t < trials && err.empty(); t++) {
            std::vector<uint32_t> a, b;
            uint32_t seed = 1000003u * (uint32_t)(t + 1) + (uint32_t)std::hash<std::string>()(name);
            bool ok;
            if (vs) { VsInputs vi = vs_inputs(seed); ok = run_vs(cf, vi, a, err) && run_vs(cd, vi, b, err); }
            else ok = run_ps(g_vs_fxc, cf, shape, seed, a, err) && run_ps(g_control >= 0 ? g_vs_fxc : g_vs_dxc, cd, shape, seed, b, err);
            if (ok) compare(a, b, s);
            collect();
        }
        for (auto& c : err) if (c == ',' || c == '\n' || c == '\r') c = ' ';
        if (csv) std::fprintf(csv, "%s,%s,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%.6g,%.6g,%s\n", name.c_str(), vs ? "vs" : "ps", s.values, s.bit_diff,
                              s.rel5, s.abs1024, s.coverage, s.nan_fxc, s.nan_dxc, s.inf, s.max_abs, s.max_rel, err.c_str());
        if (!err.empty()) {
            if (vs && err.find("invalid subscript 'vid'") != std::string::npos) { unsupported++; continue; }   // CPU-fetch VS: no vertex id
            failed++; std::printf("%s: %s\n", name.c_str(), err.c_str()); continue;
        }
        shaders[k]++;
        if (s.bit_diff) differing[k]++;
        bool vis = s.abs1024 || s.coverage || s.nan_fxc || s.nan_dxc || s.inf;
        if (vis) { visible[k]++; std::printf("%s: beyond 1/1024 rel %zu, coverage %zu, NaN only FXC %zu, NaN only DXC %zu, inf %zu, max rel %.4g\n",
                                         name.c_str(), s.abs1024, s.coverage, s.nan_fxc, s.nan_dxc, s.inf, s.max_rel); }
        Stats& T = total[k];
        T.values += s.values; T.bit_diff += s.bit_diff; T.rel5 += s.rel5; T.abs1024 += s.abs1024; T.coverage += s.coverage;
        T.nan_fxc += s.nan_fxc; T.nan_dxc += s.nan_dxc; T.inf += s.inf; T.max_abs = std::max(T.max_abs, s.max_abs); T.max_rel = std::max(T.max_rel, s.max_rel);
    }
    if (csv) std::fclose(csv);
    for (int k = 0; k < 2; k++) {
        Stats& T = total[k];
        std::printf("%s: %zu shaders, %zu with any bit difference, %zu flagged; values %zu: bit-different %zu, beyond 1e-5 rel %zu, "
                    "beyond 1/1024 rel %zu, coverage %zu, NaN only FXC %zu, NaN only DXC %zu, inf %zu, max rel %.4g\n",
                    k ? "pixel" : "vertex", shaders[k], differing[k], visible[k], T.values, T.bit_diff, T.rel5, T.abs1024, T.coverage,
                    T.nan_fxc, T.nan_dxc, T.inf, T.max_rel);
    }
    std::printf("failed %zu; CPU-fetch vertex shaders not run %zu; pixel shaders rewritten for DXC %zu\n", failed, unsupported, scalarized);
    return failed == 0 && visible[0] == 0 && visible[1] == 0 ? 0 : 1;
}
