/* Verify fallback VS + DXC guest PS PSO compatibility on WARP (#70/#71).
 * Demonstrates that pairing an FXC vs_5_0 fallback VS with a DXC ps_6_0 PS
 * fails CreateGraphicsPipelineState with E_INVALIDARG (0x80070057), while
 * compiling the fallback VS through shader_compile(shader_target("vs_5_0"))
 * succeeds. */
#define CINTERFACE
#define COBJMACROS
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <assert.h>

typedef uint64_t u64;
typedef uint8_t u8;
static struct { ID3D12Device* device; } s_d3d;
#include "compiler.inc"

static const char vs_src[] =
    "float4 main(uint id : SV_VertexID) : SV_Position {\n"
    "  return float4(id == 1 ? 3 : -1, id == 2 ? 3 : -1, 0, 1);\n"
    "}\n";

static const char ps_src[] =
    "float4 main(float4 pos : SV_Position) : SV_Target {\n"
    "  return float4(1, 0, 0, 1);\n"
    "}\n";

static HRESULT try_pso(ID3D12RootSignature* rs, ID3DBlob* vb, ID3DBlob* pb) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
    pd.pRootSignature = rs;
    pd.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(vb);
    pd.VS.BytecodeLength  = ID3D10Blob_GetBufferSize(vb);
    pd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(pb);
    pd.PS.BytecodeLength  = ID3D10Blob_GetBufferSize(pb);
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    pd.SampleDesc.Count = 1;
    ID3D12PipelineState* pso = NULL;
    HRESULT hr = ID3D12Device_CreateGraphicsPipelineState(
        s_d3d.device, &pd, &IID_ID3D12PipelineState, (void**)&pso);
    if (pso) ID3D12PipelineState_Release(pso);
    return hr;
}

int main(void) {
    IDXGIFactory4* factory = NULL;
    IDXGIAdapter* warp = NULL;
    assert(SUCCEEDED(CreateDXGIFactory1(&IID_IDXGIFactory4, (void**)&factory)));
    assert(SUCCEEDED(IDXGIFactory4_EnumWarpAdapter(factory, &IID_IDXGIAdapter, (void**)&warp)));
    assert(SUCCEEDED(D3D12CreateDevice((IUnknown*)warp, D3D_FEATURE_LEVEL_11_0,
                                       &IID_ID3D12Device, (void**)&s_d3d.device)));
    assert(shader_dxc_on() == 1);

    D3D12_ROOT_SIGNATURE_DESC rd = {0};
    ID3DBlob *root = NULL, *err = NULL;
    assert(SUCCEEDED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &root, &err)));
    ID3D12RootSignature* rs = NULL;
    assert(SUCCEEDED(ID3D12Device_CreateRootSignature(
        s_d3d.device, 0, ID3D10Blob_GetBufferPointer(root), ID3D10Blob_GetBufferSize(root),
        &IID_ID3D12RootSignature, (void**)&rs)));

    /* Old compile_vp path: FXC vs_5_0 blob */
    ID3DBlob *vb_fxc = NULL, *vb_dxc = NULL, *pb_dxc = NULL;
    assert(SUCCEEDED(D3DCompile(vs_src, sizeof(vs_src) - 1, "vp", NULL, NULL,
                                "main", "vs_5_0", 0, 0, &vb_fxc, &err)) && vb_fxc);
    if (err) { ID3D10Blob_Release(err); err = NULL; }

    /* New compile_vp fallback path + guest FP path: DXC SM 6.0 */
    assert(SUCCEEDED(shader_compile(vs_src, sizeof(vs_src) - 1, "vp",
                                    shader_target("vs_5_0"), D3DCOMPILE_IEEE_STRICTNESS,
                                    &vb_dxc, &err)) && vb_dxc);
    if (err) { ID3D10Blob_Release(err); err = NULL; }
    assert(SUCCEEDED(shader_compile(ps_src, sizeof(ps_src) - 1, "guest_fp",
                                    shader_target("ps_5_0"), D3DCOMPILE_IEEE_STRICTNESS,
                                    &pb_dxc, &err)) && pb_dxc);
    if (err) { ID3D10Blob_Release(err); err = NULL; }

    /* Reproduces #70/#71: FXC vs_5_0 + DXC ps_6_0 fails with E_INVALIDARG (0x80070057). */
    HRESULT hr_bad = try_pso(rs, vb_fxc, pb_dxc);
    assert(hr_bad == E_INVALIDARG);

    /* Fixed fallback: DXC vs_6_0 + DXC ps_6_0 succeeds. */
    HRESULT hr_ok = try_pso(rs, vb_dxc, pb_dxc);
    assert(SUCCEEDED(hr_ok));

    ID3D10Blob_Release(vb_fxc);
    ID3D10Blob_Release(vb_dxc);
    ID3D10Blob_Release(pb_dxc);
    ID3D12RootSignature_Release(rs);
    ID3D10Blob_Release(root);
    ID3D12Device_Release(s_d3d.device);
    IDXGIAdapter_Release(warp);
    IDXGIFactory4_Release(factory);
    puts("PASS: FXC fallback VS + DXC PS fails E_INVALIDARG; DXC fallback VS + DXC PS succeeds");
    return 0;
}
