/* Production compiler block, signed DXIL, minimal resource roots and concurrent calls. */
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
static int dxc;
static const char vs[] = "float4 main(uint id:SV_VertexID):SV_Position{return float4(id==1?3:-1,id==2?3:-1,0,1);}";
static const char ps[] =
    "Texture2D rsx_tex[16] : register(t0);\n"
    "SamplerState rsx_samp[16] : register(s0);\n"
    "float4 main(float4 pos:SV_Position):SV_Target{return rsx_tex[3].Sample(rsx_samp[3],pos.xy/64);}";
static ID3DBlob* compile(const char* src, const char* target) {
    ID3DBlob *out=NULL,*err=NULL;
    HRESULT hr=shader_compile(src,strlen(src),"fixture",shader_target(target),0,&out,&err);
    if (FAILED(hr) && err) fprintf(stderr,"%s\n",(char*)ID3D10Blob_GetBufferPointer(err));
    assert(SUCCEEDED(hr)&&out);if(err)ID3D10Blob_Release(err);
    uint8_t* p=ID3D10Blob_GetBufferPointer(out);size_t n=ID3D10Blob_GetBufferSize(out);
    assert(n>32&&!memcmp(p,"DXBC",4));
    uint32_t chunks;memcpy(&chunks,p+28,4);int dxil=0;
    for(uint32_t i=0;i<chunks;i++){uint32_t off;memcpy(&off,p+32+4*i,4);assert(off+8<n);if(!memcmp(p+off,"DXIL",4))dxil=1;}
    assert(dxil==dxc);
    if(dxc){int signed_hash=0;for(int i=4;i<20;i++)signed_hash|=p[i];assert(signed_hash);}
    return out;
}
static DWORD WINAPI worker(void* unused) {
    (void)unused;
    for(int i=0;i<8;i++) {assert(shader_dxc_on()==dxc);ID3DBlob* b=compile(vs,"vs_5_0");ID3D10Blob_Release(b);}
    return 0;
}
int main(int argc,char** argv) {
    assert(argc==3);dxc=atoi(argv[1]);
    IDXGIFactory4* factory=NULL;IDXGIAdapter* warp=NULL;
    assert(SUCCEEDED(CreateDXGIFactory1(&IID_IDXGIFactory4,(void**)&factory)));
    assert(SUCCEEDED(IDXGIFactory4_EnumWarpAdapter(factory,&IID_IDXGIAdapter,(void**)&warp)));
    assert(SUCCEEDED(D3D12CreateDevice((IUnknown*)warp,D3D_FEATURE_LEVEL_11_0,&IID_ID3D12Device,(void**)&s_d3d.device)));
    HANDLE threads[8];for(int i=0;i<8;i++)threads[i]=CreateThread(NULL,0,worker,NULL,0,NULL);
    assert(WaitForMultipleObjects(8,threads,TRUE,30000)==WAIT_OBJECT_0);
    for(int i=0;i<8;i++)CloseHandle(threads[i]);
    assert(shader_dxc_on()==dxc);
    assert(strstr(shader_cache_dir(),"dxc-1.8.2502-sm60-v1")!=NULL==dxc);
    assert(!strcmp(getenv("RSX_SHADER_CACHE"),argv[2]));
    assert(shader_source_key(vs,strlen(vs),"vs_5_0",0)!=shader_source_key(vs,strlen(vs),"vs_6_0",D3DCOMPILE_IEEE_STRICTNESS));
    ID3DBlob *vb=compile(vs,"vs_5_0"),*pb=compile(ps,"ps_5_0"),*again=compile(ps,"ps_5_0");
    assert(ID3D10Blob_GetBufferSize(pb)==ID3D10Blob_GetBufferSize(again));
    assert(!memcmp(ID3D10Blob_GetBufferPointer(pb),ID3D10Blob_GetBufferPointer(again),ID3D10Blob_GetBufferSize(pb)));
    ID3D10Blob_Release(again);
    D3D12_DESCRIPTOR_RANGE range={D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,3,0,0};
    D3D12_ROOT_PARAMETER param={0};param.ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    param.DescriptorTable.NumDescriptorRanges=1;param.DescriptorTable.pDescriptorRanges=&range;param.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler={0};sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxAnisotropy=1;sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD=D3D12_FLOAT32_MAX;sampler.ShaderRegister=3;sampler.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rd={1,&param,1,&sampler,0};ID3DBlob *root=NULL,*err=NULL;
    assert(SUCCEEDED(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&root,&err)));
    ID3D12RootSignature* rs=NULL;
    assert(SUCCEEDED(ID3D12Device_CreateRootSignature(s_d3d.device,0,ID3D10Blob_GetBufferPointer(root),ID3D10Blob_GetBufferSize(root),&IID_ID3D12RootSignature,(void**)&rs)));
    D3D12_GRAPHICS_PIPELINE_STATE_DESC p={0};p.pRootSignature=rs;
    p.VS=(D3D12_SHADER_BYTECODE){ID3D10Blob_GetBufferPointer(vb),ID3D10Blob_GetBufferSize(vb)};
    p.PS=(D3D12_SHADER_BYTECODE){ID3D10Blob_GetBufferPointer(pb),ID3D10Blob_GetBufferSize(pb)};
    p.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;p.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
    p.RasterizerState.DepthClipEnable=TRUE;p.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
    p.SampleMask=UINT_MAX;p.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    p.NumRenderTargets=1;p.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM;p.SampleDesc.Count=1;
    ID3D12PipelineState* pipeline=NULL;
    HRESULT hr=ID3D12Device_CreateGraphicsPipelineState(s_d3d.device,&p,&IID_ID3D12PipelineState,(void**)&pipeline);
    if(FAILED(hr))fprintf(stderr,"minimal root PSO: %08lx\n",hr);
    assert(SUCCEEDED(hr));ID3D12PipelineState_Release(pipeline);
    ID3DBlob* bad=NULL;err=NULL;
    hr=shader_compile("invalid shader",14,"bad",shader_target("ps_5_0"),0,&bad,&err);
    assert(FAILED(hr)&&!bad&&err);ID3D10Blob_Release(err);
    ID3D12RootSignature_Release(rs);ID3D10Blob_Release(root);ID3D10Blob_Release(vb);ID3D10Blob_Release(pb);
    ID3D12Device_Release(s_d3d.device);IDXGIAdapter_Release(warp);IDXGIFactory4_Release(factory);
    puts("PASS: compiler, signed container, concurrent init, namespace, minimal root PSO, errors");
    return 0;
}
