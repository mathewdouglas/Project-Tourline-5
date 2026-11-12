/* GPU/driver stamp and the startup preparation before the game (GT5 Phase 44),
 * end to end on D3D12 WARP. Synthetic pipelines only. gpu_stamp.py extracts
 * the real backend from its hashing helpers to the start of
 * rsx_d3d12_backend_prewarm and runs this once per phase, each a fresh
 * process (the stamp is read once a run), over one scratch cache:
 *   learn    stamp A: 48 pipelines, blobs, menu (16) and race (32) indexes, pack
 *   same     stamp A: blobs read; before-start pass on 6 threads, from the pack
 *   changed  stamp B: blobs and pack skipped unread, rebuilt; pass forced
 *   after    stamp B: the rebuilt blobs are read again
 *   off      RSX_PSO_GPU_STAMP=0 with stamp C: nothing skipped, file kept
 *   empty    nothing learned: the pass declines and the guest is not held */
#define CINTERFACE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
typedef uint8_t u8; typedef uint32_t u32; typedef uint64_t u64;
#define CHECK(c) do {if(!(c)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#c);exit(1);}}while(0)
static struct {ID3D12Device* device;ID3D12RootSignature *vp_root_sig,*root_signature;u64 frame_count;} s_d3d;
static unsigned long long s_vp_rs_hash=0x1234,s_rs_hash=0x5678;
static volatile LONG g_rsx_shaders_pending,g_rsx_shader_busy,g_rsx_shader_preload_busy,g_rsx_pipelines_built;
static volatile LONG preload_work,demand_work;
static volatile LONG64 s_driver_pso_calls,s_driver_pso_cached,s_driver_pso_us,s_driver_pso_max_us;
static __declspec(thread) int t_shader_preload;
static volatile LONG* shader_busy_ctr(int preload){return preload?&g_rsx_shader_preload_busy:&g_rsx_shader_busy;}
static void shader_work_done(int preload){InterlockedIncrement(preload?&preload_work:&demand_work);}
static int shc_async_on(void){return 1;}
static const char* shader_cache_dir(void){return getenv("RSX_SHADER_CACHE");}
static void ps3_name_thread(const wchar_t* s){(void)s;}
static void spack_race_begin(void){}
static void spack_race_end(void){}
static volatile LONG s_spack_active=1;
static void spack_begin(int scope){(void)scope;}
#include "backend.inc"

enum {N=48,MENU=16,SHADERS=3};
static D3D12_GRAPHICS_PIPELINE_STATE_DESC descs[N];
static D3D12_INPUT_ELEMENT_DESC input;
static u64 hashes[N];
static char cache[600],warm[640];

static ID3DBlob* compile(const char* text,const char* target){
    ID3DBlob *blob=NULL,*err=NULL;
    HRESULT hr=D3DCompile(text,strlen(text),NULL,NULL,NULL,"main",target,0,0,&blob,&err);
    if(err){fwrite(err->lpVtbl->GetBufferPointer(err),1,err->lpVtbl->GetBufferSize(err),stderr);err->lpVtbl->Release(err);}
    CHECK(SUCCEEDED(hr));return blob;
}
static int resident(u64 hash){ID3D12PipelineState* p=NULL;if(!pso_warm_get(hash,&p))return 0;p->lpVtbl->Release(p);return 1;}
static u32 resident_count(void){u32 c=0;for(u32 i=0;i<N;i++)c+=resident(hashes[i]);return c;}
static void write_index(const char* name,const u64* list,u32 n){
    char p[700];snprintf(p,sizeof p,"%s/%s",warm,name);FILE* f=fopen(p,"w");CHECK(f);
    for(u32 i=0;i<n;i++)fprintf(f,"%016llx\n",(unsigned long long)list[i]);
    CHECK(!fclose(f));
}
static int file_exists(const char* p){return GetFileAttributesA(p)!=INVALID_FILE_ATTRIBUTES;}
static u64 mtime(const char* p){
    WIN32_FILE_ATTRIBUTE_DATA a;CHECK(GetFileAttributesExA(p,GetFileExInfoStandard,&a));return filetime_unix(&a.ftLastWriteTime);
}
static void read_stamp(char* stamp,size_t n,unsigned long long* since){
    char p[700],line[512];snprintf(p,sizeof p,"%s/gpu-stamp.txt",cache);
    FILE* f=fopen(p,"r");CHECK(f);stamp[0]=0;*since=~0ull;
    while(fgets(line,sizeof line,f)){
        line[strcspn(line,"\r\n")]=0;
        if(!strncmp(line,"stamp=",6))snprintf(stamp,n,"%s",line+6);
        else if(!strncmp(line,"since=",6))*since=strtoull(line+6,NULL,10);
    }
    fclose(f);
}
static void blob_path(char* p,size_t n,u32 i){snprintf(p,n,"%s/pso_%016llx.bin",cache,(unsigned long long)hashes[i]);}
static void before_start(int forced,u32 want_threads){
    char t[16];snprintf(t,sizeof t,"%u",want_threads);_putenv_s("RSX_SHADER_WARMUP_THREADS",t);
    CHECK(pso_startup_threads()==(int)want_threads);
    CHECK(pso_startup_parallel(warm,forced)==1);
    CHECK(resident_count()==N && s_sp_ready==N && !s_sp_rejected);
    CHECK(g_rsx_startup_prep_done==N && g_rsx_startup_prep_total==0);
    CHECK(!s_pack_bytes && !s_sp_pack && !g_rsx_shader_preload_busy);
    CHECK(s_recipes_maintained);
}

int main(int argc,char** argv){
    CHECK(argc==3);
    const char* mode=argv[2];
    snprintf(cache,sizeof cache,"%s",argv[1]);snprintf(warm,sizeof warm,"%s/warmup-v1",cache);
    _putenv_s("RSX_SHADER_CACHE",cache);
    _putenv_s("RSX_PSO_RECIPE_KEEP_DAYS","0");
    _putenv_s("RSX_PSO_HITCH_MS","0");
    IDXGIFactory4* factory=NULL;IDXGIAdapter* adapter=NULL;
    CHECK(SUCCEEDED(CreateDXGIFactory1(&IID_IDXGIFactory4,(void**)&factory)));
    CHECK(SUCCEEDED(factory->lpVtbl->EnumWarpAdapter(factory,&IID_IDXGIAdapter,(void**)&adapter)));
    CHECK(SUCCEEDED(D3D12CreateDevice((IUnknown*)adapter,D3D_FEATURE_LEVEL_11_0,&IID_ID3D12Device,(void**)&s_d3d.device)));
    adapter->lpVtbl->Release(adapter);factory->lpVtbl->Release(factory);
    D3D12_ROOT_SIGNATURE_DESC root={0};root.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3DBlob* serialized=NULL;CHECK(SUCCEEDED(D3D12SerializeRootSignature(&root,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,NULL)));
    CHECK(SUCCEEDED(s_d3d.device->lpVtbl->CreateRootSignature(s_d3d.device,0,serialized->lpVtbl->GetBufferPointer(serialized),serialized->lpVtbl->GetBufferSize(serialized),&IID_ID3D12RootSignature,(void**)&s_d3d.vp_root_sig)));
    serialized->lpVtbl->Release(serialized);
    ID3DBlob* vs=compile("float4 main(float4 p:POSITION):SV_POSITION{return p;}","vs_5_0");
    ID3DBlob* ps[SHADERS];
    ps[0]=compile("float4 main():SV_TARGET{return float4(1,0,0,1);}","ps_5_0");
    ps[1]=compile("float4 main():SV_TARGET{return float4(0,1,0,1);}","ps_5_0");
    ps[2]=compile("float4 main(float4 p:SV_POSITION):SV_TARGET{return float4(frac(p.xy*0.1),1,1);}","ps_5_0");
    input.SemanticName="POSITION";input.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;
    for(u32 i=0;i<N;i++){
        D3D12_GRAPHICS_PIPELINE_STATE_DESC* d=&descs[i];
        d->pRootSignature=s_d3d.vp_root_sig;
        d->VS.pShaderBytecode=vs->lpVtbl->GetBufferPointer(vs);d->VS.BytecodeLength=vs->lpVtbl->GetBufferSize(vs);
        d->PS.pShaderBytecode=ps[i%SHADERS]->lpVtbl->GetBufferPointer(ps[i%SHADERS]);d->PS.BytecodeLength=ps[i%SHADERS]->lpVtbl->GetBufferSize(ps[i%SHADERS]);
        d->InputLayout.pInputElementDescs=&input;d->InputLayout.NumElements=1;
        d->RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;d->RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
        d->RasterizerState.DepthBias=(INT)i;
        d->BlendState.RenderTarget[0].RenderTargetWriteMask=15;
        d->SampleMask=~0u;d->SampleDesc.Count=1;d->PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        d->NumRenderTargets=1;d->RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM;
        hashes[i]=guest_pso_hash(d);
    }
    char stamp[400],p[800];unsigned long long since=0;int forced=0;
    char packp[700];snprintf(packp,sizeof packp,"%s/race-pack-v1.bin",warm);

    if(!strcmp(mode,"learn")){
        snprintf(s_gpu_stamp,sizeof s_gpu_stamp,"vendor=1414 device=008C gpu=WARP A");
        for(u32 i=0;i<N;i++){ID3D12PipelineState* q=NULL;CHECK(SUCCEEDED(create_guest_pso_cached(&descs[i],&q))&&q);q->lpVtbl->Release(q);}
        blob_path(p,sizeof p,0);CHECK(file_exists(p));   /* WARP hands out cached blobs */
        read_stamp(stamp,sizeof stamp,&since);CHECK(!strcmp(stamp,s_gpu_stamp) && since==0 && !s_gpu_changed && !s_blob_since);
        write_index("menu-v1.txt",hashes,MENU);write_index("race-v1.txt",hashes+MENU,N-MENU);
        pso_warm_clear();
        /* A race load: helpers prepare from files, then the pack is written. */
        pso_race_notify("PitSceneRoot.gpb");pso_race_notify("LoadingRoot.gpb");
        CHECK(WaitForSingleObject(s_race_ready,0)==WAIT_OBJECT_0);
        ULONGLONG began=GetTickCount64();
        while(s_pack_stored==0){CHECK(GetTickCount64()-began<20000);Sleep(2);}
        pso_queue_stop();
        CHECK(file_exists(packp));
        puts("PASS: learn: stamp recorded with since=0; 48 blobs, menu/race indexes and race pack written");
        return 0;
    }
    if(!strcmp(mode,"same")){
        snprintf(s_gpu_stamp,sizeof s_gpu_stamp,"vendor=1414 device=008C gpu=WARP A");
        CHECK(!pso_startup_parallel_on(&forced) && !forced);   /* default: background mode */
        _putenv_s("RSX_SHADER_WARMUP_STARTUP","1");CHECK(pso_startup_parallel_on(&forced) && !forced);
        CHECK(!s_gpu_changed && !s_blob_since);
        const LONG64 cached=s_driver_pso_cached;
        before_start(0,6);
        CHECK(s_pack_hits==1 && s_driver_pso_cached-cached==N-MENU+MENU && !s_blob_stale);
        puts("PASS: same driver: the before-start pass prepares 16 menu + 32 race (pack) on 6 threads; every blob is used");
        return 0;
    }
    if(!strcmp(mode,"changed")){
        snprintf(s_gpu_stamp,sizeof s_gpu_stamp,"vendor=1414 device=008C gpu=WARP B");
        _putenv_s("RSX_PSO_GPU_STAMP_PRELOAD","0");CHECK(!pso_startup_parallel_on(&forced) && !forced);
        _putenv_s("RSX_PSO_GPU_STAMP_PRELOAD","");CHECK(pso_startup_parallel_on(&forced) && forced);
        CHECK(s_gpu_changed && s_blob_since && !strcmp(s_gpu_stamp_old,"vendor=1414 device=008C gpu=WARP A"));
        read_stamp(stamp,sizeof stamp,&since);CHECK(!strcmp(stamp,s_gpu_stamp) && since==s_blob_since);
        CHECK(pso_file_predates_driver(packp));
        const LONG64 cached=s_driver_pso_cached;
        before_start(1,3);
        CHECK(!s_pack_hits && s_driver_pso_cached==cached && s_blob_stale==N);   /* nothing old was read or offered */
        for(u32 i=0;i<N;i++){blob_path(p,sizeof p,i);CHECK(mtime(p)>=since && !pso_file_predates_driver(p));}
        puts("PASS: changed driver: forced before-start pass; 48 old blobs and the old pack skipped unread, blobs rebuilt");
        return 0;
    }
    if(!strcmp(mode,"after")){
        snprintf(s_gpu_stamp,sizeof s_gpu_stamp,"vendor=1414 device=008C gpu=WARP B");
        CHECK(!pso_startup_parallel_on(&forced));
        read_stamp(stamp,sizeof stamp,&since);CHECK(!s_gpu_changed && s_blob_since==since && since);
        _putenv_s("RSX_SHADER_WARMUP_STARTUP","1");
        const LONG64 cached=s_driver_pso_cached;
        before_start(0,4);
        CHECK(!s_pack_hits && s_driver_pso_cached-cached==N && !s_blob_stale);   /* the pack is still the old one */
        /* The first race load writes a pack for this driver. */
        pso_race_notify("PitSceneRoot.gpb");pso_race_notify("LoadingRoot.gpb");
        ULONGLONG began=GetTickCount64();
        while(s_pack_stored==0){CHECK(GetTickCount64()-began<20000);Sleep(2);}
        pso_queue_stop();
        CHECK(!pso_file_predates_driver(packp));
        puts("PASS: next run on the new driver: rebuilt blobs are read again; the old pack is rewritten by the first race load");
        return 0;
    }
    if(!strcmp(mode,"off")){
        snprintf(s_gpu_stamp,sizeof s_gpu_stamp,"vendor=1414 device=008C gpu=WARP C");
        _putenv_s("RSX_PSO_GPU_STAMP","0");
        read_stamp(stamp,sizeof stamp,&since);
        CHECK(!pso_startup_parallel_on(&forced) && !s_gpu_changed && !s_blob_since);
        char stamp2[400];unsigned long long since2;read_stamp(stamp2,sizeof stamp2,&since2);
        CHECK(!strcmp(stamp,stamp2) && since==since2);
        blob_path(p,sizeof p,0);CHECK(!pso_file_predates_driver(p) && !pso_file_predates_driver(packp));
        puts("PASS: RSX_PSO_GPU_STAMP=0: stamp file untouched, nothing skipped, nothing forced");
        return 0;
    }
    if(!strcmp(mode,"empty")){
        snprintf(s_gpu_stamp,sizeof s_gpu_stamp,"vendor=1414 device=008C gpu=WARP A");
        _putenv_s("RSX_SHADER_WARMUP_STARTUP","1");
        CHECK(pso_startup_parallel_on(&forced));
        CHECK(pso_startup_parallel(warm,0)==0 && !g_rsx_startup_prep_total && !s_pack_bytes);
        puts("PASS: nothing learned: the before-start pass declines (background mode takes over)");
        return 0;
    }
    CHECK(!"unknown mode");
    return 1;
}
