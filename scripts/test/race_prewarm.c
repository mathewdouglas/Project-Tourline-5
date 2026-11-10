/* Race pack and helper-thread prewarm, end to end on D3D12 WARP.
 * Synthetic pipelines only. Unlike pso_async.c nothing below the driver is a
 * stand-in: the extracted backend block (race_prewarm.py) holds the real
 * hashing, driver-blob cache, recipe files, queue, pack and helper code. */
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
static void ps3_name_thread(const wchar_t* s){(void)s;}
static void spack_race_begin(void){}
static void spack_race_end(void){}
static volatile LONG s_spack_active=1;
static void spack_begin(int scope){(void)scope;}
#include "backend.inc"

enum {N=48,SHADERS=3};
static D3D12_GRAPHICS_PIPELINE_STATE_DESC descs[N+1];
static D3D12_INPUT_ELEMENT_DESC input;
static u64 hashes[N+1];
static char cache[600],warm[640],indexp[700],packp[700];

static ID3DBlob* compile(const char* text,const char* target){
    ID3DBlob *blob=NULL,*err=NULL;
    HRESULT hr=D3DCompile(text,strlen(text),NULL,NULL,NULL,"main",target,0,0,&blob,&err);
    if(err){fwrite(err->lpVtbl->GetBufferPointer(err),1,err->lpVtbl->GetBufferSize(err),stderr);err->lpVtbl->Release(err);}
    CHECK(SUCCEEDED(hr));return blob;
}
static int resident(u64 hash){
    ID3D12PipelineState* p=NULL;if(!pso_warm_get(hash,&p))return 0;p->lpVtbl->Release(p);return 1;
}
static u32 resident_count(u32 n){u32 c=0;for(u32 i=0;i<n;i++)c+=resident(hashes[i]);return c;}
static void write_index(const u64* list,u32 n){
    FILE* f=fopen(indexp,"w");CHECK(f);
    for(u32 i=0;i<n;i++)fprintf(f,"%016llx\n",(unsigned long long)list[i]);
    CHECK(!fclose(f));
}
static unsigned char* slurp(const char* path,size_t* n){
    FILE* f=fopen(path,"rb");if(!f)return NULL;
    fseek(f,0,SEEK_END);*n=(size_t)ftell(f);rewind(f);
    unsigned char* p=(unsigned char*)malloc(*n?*n:1);CHECK(p && fread(p,1,*n,f)==*n);fclose(f);return p;
}
static void spit(const char* path,const unsigned char* p,size_t n){
    FILE* f=fopen(path,"wb");CHECK(f && fwrite(p,1,n,f)==n);CHECK(!fclose(f));
}
static u32 pack_count(void){
    size_t n=0;unsigned char* p=slurp(packp,&n);if(!p)return 0;
    u32 c=0;if(n>=40)memcpy(&c,p+12,4);free(p);return c;
}
static int file_exists(const char* p){return GetFileAttributesA(p)!=INVALID_FILE_ATTRIBUTES;}
/* A fresh process state over the same cache directory. */
static void session_end(void){
    pso_race_notify("TopMenuTemplate.gpb");
    pso_queue_stop();pso_warm_clear();
    CHECK(!s_pack_bytes && !s_pack_items && !s_prewarm_thr_n);
    s_pso_stopping=0;s_race_head=s_race_tail=0;
    memset(s_race_seen,0,sizeof s_race_seen);memset(s_menu_seen,0,sizeof s_menu_seen);
    s_race_loaded=s_race_rejected=0;s_pack_stale_rewrote=0;s_pso_warm_loaded=s_pso_warm_rejected=0;
    InterlockedExchange(&s_pack_hits,0);
}
/* The pack bytes are dropped by the worker shortly after the gate opens. */
static void wait_released(void){
    ULONGLONG began=GetTickCount64();
    while(s_pack_bytes || s_prewarm_helpers){CHECK(GetTickCount64()-began<10000);Sleep(2);}
}
static void wait_stored(LONG before){
    ULONGLONG began=GetTickCount64();
    while(s_pack_stored==before){CHECK(GetTickCount64()-began<10000);Sleep(2);}
}
static void load_race(void){
    pso_race_notify("PitSceneRoot.gpb");pso_race_notify("LoadingRoot.gpb");
    CHECK(WaitForSingleObject(s_race_ready,0)==WAIT_OBJECT_0);
}
static void hide_files(int hide){
    /* Move every per-file input away (or back): a pack that hits needs none. */
    char pattern[700],from[900],to[900];WIN32_FIND_DATAA fd;
    const char* dirs[2]={warm,cache};
    for(int d=0;d<2;d++){
        snprintf(pattern,sizeof pattern,"%s/%s",dirs[d],hide?"*.*":"*.hidden");
        HANDLE h=FindFirstFileA(pattern,&fd);if(h==INVALID_HANDLE_VALUE)continue;
        do{
            if(fd.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)continue;
            const char* ext=strrchr(fd.cFileName,'.');if(!ext)continue;
            snprintf(from,sizeof from,"%s/%s",dirs[d],fd.cFileName);
            if(hide){
                if(strcmp(ext,".recipe")&&strcmp(ext,".dxbc")&&(strcmp(ext,".bin")||!strncmp(fd.cFileName,"race-pack",9)))continue;
                snprintf(to,sizeof to,"%s.hidden",from);
            }else{snprintf(to,sizeof to,"%s",from);to[strlen(to)-7]=0;}
            CHECK(MoveFileExA(from,to,MOVEFILE_REPLACE_EXISTING));
        }while(FindNextFileA(h,&fd));
        FindClose(h);
    }
}
static DWORD WINAPI load_thread(LPVOID unused){(void)unused;pso_race_notify("LoadingRoot.gpb");return 0;}

int main(int argc,char** argv){
    CHECK(argc==2);
    snprintf(cache,sizeof cache,"%s",argv[1]);snprintf(warm,sizeof warm,"%s/warmup-v1",cache);
    snprintf(indexp,sizeof indexp,"%s/race-v1.txt",warm);snprintf(packp,sizeof packp,"%s/race-pack-v1.bin",warm);
    _putenv_s("RSX_SHADER_CACHE",cache);
    _putenv_s("RSX_PSO_RECIPE_KEEP_DAYS","0");   /* this test owns the index order and its repeats */
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
    for(u32 i=0;i<=N;i++){
        D3D12_GRAPHICS_PIPELINE_STATE_DESC* d=&descs[i];
        d->pRootSignature=s_d3d.vp_root_sig;
        d->VS.pShaderBytecode=vs->lpVtbl->GetBufferPointer(vs);d->VS.BytecodeLength=vs->lpVtbl->GetBufferSize(vs);
        d->PS.pShaderBytecode=ps[i%SHADERS]->lpVtbl->GetBufferPointer(ps[i%SHADERS]);d->PS.BytecodeLength=ps[i%SHADERS]->lpVtbl->GetBufferSize(ps[i%SHADERS]);
        d->InputLayout.pInputElementDescs=&input;d->InputLayout.NumElements=1;
        d->RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;d->RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
        d->RasterizerState.DepthBias=(INT)i;   /* one pipeline state per member */
        d->BlendState.RenderTarget[0].RenderTargetWriteMask=15;
        d->SampleMask=~0u;d->SampleDesc.Count=1;d->PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        d->NumRenderTargets=1;d->RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM;
        hashes[i]=guest_pso_hash(d);
        for(u32 k=0;k<i;k++)CHECK(hashes[k]!=hashes[i]);
    }

    /* Learn: the creation path a draw uses writes recipe, bytecode and blob files. */
    for(u32 i=0;i<N;i++){
        ID3D12PipelineState* p=NULL;CHECK(SUCCEEDED(create_guest_pso_cached(&descs[i],&p))&&p);p->lpVtbl->Release(p);
    }
    CHECK(resident_count(N)==N);pso_warm_clear();CHECK(resident_count(N)==0);
    char probe[800];snprintf(probe,sizeof probe,"%s/pso_%016llx.bin",cache,(unsigned long long)hashes[0]);
    const int blobs=file_exists(probe);   /* WARP may not hand out cached blobs; the pack then holds none */
    write_index(hashes,N);CHECK(!file_exists(packp));
    puts(blobs?"PASS: learned 48 pipelines with recipes, shared bytecode and driver blobs":
               "PASS: learned 48 pipelines with recipes and shared bytecode (this driver returns no cached blobs)");

    /* 1. Rollback switches: one recipe per worker turn, no pack read or written. */
    _putenv_s("RSX_RACE_PREWARM_THREADS","1");_putenv_s("RSX_RACE_PREWARM_PACK","0");
    load_race();CHECK(resident_count(N)==N && s_race_loaded==N && !s_race_rejected);
    CHECK(g_rsx_race_load_done==N && g_rsx_race_load_total==N && !g_rsx_race_load_waiting);
    CHECK(!s_prewarm_helpers && !s_pack_hits && !file_exists(packp));
    session_end();CHECK(!file_exists(packp));
    puts("PASS: RSX_RACE_PREWARM_THREADS=1 with RSX_RACE_PREWARM_PACK=0 keeps the single-thread per-file path and writes nothing");

    /* 2. Read-only switch: helpers from files, still no pack on disk. */
    _putenv_s("RSX_RACE_PREWARM_THREADS","4");_putenv_s("RSX_RACE_PREWARM_PACK","1");_putenv_s("RSX_RACE_PREWARM_PACK_WRITE","0");
    load_race();CHECK(resident_count(N)==N && s_race_loaded==N);wait_released();
    session_end();CHECK(!file_exists(packp));
    _putenv_s("RSX_RACE_PREWARM_PACK_WRITE","");
    puts("PASS: RSX_RACE_PREWARM_PACK_WRITE=0 prepares on helpers without writing a pack");

    /* 3. Defaults, no pack yet: four helpers from files, then the pack is written. */
    _putenv_s("RSX_RACE_PREWARM_THREADS","");_putenv_s("RSX_RACE_PREWARM_PACK","");
    CHECK(pso_prewarm_threads()==4 && pso_pack_enabled() && pso_pack_write_enabled());
    LONG stored=s_pack_stored;
    load_race();CHECK(resident_count(N)==N && s_race_loaded==N && !s_race_rejected);
    CHECK(g_rsx_race_load_done==N && g_rsx_race_load_total==N);
    wait_stored(stored);wait_released();CHECK(pack_count()==N && !s_pack_hits);
    CHECK(preload_work>=N);   /* helper work is counted as preload, never as a waiting draw's */
    const LONG demand_before=demand_work;
    session_end();
    puts("PASS: default four helpers prepare the set from files, count it as preload and write race-pack-v1.bin");

    /* 4. Pack hit with every per-file input moved away: the pack alone prepares the set. */
    hide_files(1);
    const LONG64 calls=s_driver_pso_calls,cached=s_driver_pso_cached;stored=s_pack_stored;
    load_race();
    CHECK(s_pack_hits==1 && resident_count(N)==N && s_race_loaded==N && !s_race_rejected);
    CHECK(s_driver_pso_calls-calls>=N);
    if(blobs)CHECK(s_driver_pso_cached-cached==N && s_driver_pso_calls-calls==N && !s_pack_stale);
    wait_released();CHECK(s_pack_stored==stored || !blobs);
    CHECK(demand_work==demand_before);
    session_end();hide_files(0);
    puts("PASS: a matching pack prepares all 48 with no recipe, bytecode or blob file present, then its bytes are released");

    if(blobs){
        /* 5. Same membership in another order, with repeated index lines: still a hit, in the live order. */
        u64 shuffled[N+8];for(u32 i=0;i<N;i++)shuffled[i]=hashes[N-1-i];
        for(u32 i=0;i<8;i++)shuffled[N+i]=hashes[i*3];
        write_index(shuffled,N+8);
        _putenv_s("RSX_RACE_PREWARM_THREADS","1");_putenv_s("RSX_RACE_PACK_KEEP","1");
        stored=s_pack_stored;
        load_race();CHECK(s_pack_hits==1 && resident_count(N)==N && s_race_loaded==N && s_pack_stored==stored);
        CHECK(s_pack_items && s_pack_n==N);for(u32 i=0;i<N;i++)CHECK(s_pack_items[i].hash==shuffled[i]);
        CHECK(s_pack_bytes);   /* RSX_RACE_PACK_KEEP=1 */
        session_end();_putenv_s("RSX_RACE_PACK_KEEP","");_putenv_s("RSX_RACE_PREWARM_THREADS","");
        puts("PASS: reordered index with repeated lines still hits; items follow the live order; single-thread pack walk and RSX_RACE_PACK_KEEP work");

        /* 6. Membership changed: no hit, per-file fallback, pack rewritten for the new set. */
        write_index(hashes,N-5);stored=s_pack_stored;
        load_race();CHECK(!s_pack_hits && resident_count(N)==N-5 && s_race_loaded==N-5);
        wait_stored(stored);wait_released();CHECK(pack_count()==N-5);
        session_end();
        write_index(hashes,N);stored=s_pack_stored;
        load_race();CHECK(!s_pack_hits && resident_count(N)==N);wait_stored(stored);wait_released();CHECK(pack_count()==N);
        session_end();
        puts("PASS: a changed race index rejects the old pack, prepares from files and rewrites the pack");

        /* 7. Damaged pack bytes: payload checksum rejects it, files serve the load, pack is rewritten. */
        size_t n=0;unsigned char* good=slurp(packp,&n);CHECK(good && n>4096);
        unsigned char* bad=(unsigned char*)malloc(n);memcpy(bad,good,n);bad[n-100]^=0x40;spit(packp,bad,n);
        stored=s_pack_stored;
        load_race();CHECK(!s_pack_hits && resident_count(N)==N && !s_race_rejected);wait_stored(stored);wait_released();
        session_end();
        size_t n2=0;unsigned char* again=slurp(packp,&n2);CHECK(again && n2==n && !memcmp(again,good,n));free(again);
        /* Truncated, and an absurd count in the header. */
        spit(packp,good,n/2);load_race();CHECK(!s_pack_hits && resident_count(N)==N);wait_released();session_end();
        memcpy(bad,good,n);{u32 huge=0x7fffffffu;memcpy(bad+12,&huge,4);}spit(packp,bad,n);
        load_race();CHECK(!s_pack_hits && resident_count(N)==N);wait_released();session_end();
        puts("PASS: flipped, truncated and bad-header packs are rejected; the rewrite reproduces the original bytes");

        /* 8. A driver blob the driver refuses (as after a driver update), with valid checksums:
         *    the member is rebuilt through the per-file cache and the pack rewritten once. */
        again=slurp(packp,&n2);CHECK(again && n2==n);
        u32 count=0,codes=0;memcpy(&count,again+12,4);memcpy(&codes,again+16,4);CHECK(count==N);
        size_t tables=40u+(size_t)count*32u+(size_t)codes*16u;
        u32 blob_off=0,blob_len=0;memcpy(&blob_off,again+40+7*32+24,4);memcpy(&blob_len,again+40+7*32+28,4);CHECK(blob_len>32);
        memset(again+tables+blob_off,0xA5,blob_len);
        u64 fnv=pso_hash_bytes(1469598103934665603ull,again+tables,n-tables);memcpy(again+32,&fnv,8);
        spit(packp,again,n);free(again);
        stored=s_pack_stored;
        load_race();CHECK(s_pack_hits==1 && resident_count(N)==N && !s_race_rejected && s_pack_stale==1);
        wait_stored(stored);wait_released();session_end();
        again=slurp(packp,&n2);CHECK(again && n2==n && !memcmp(again,good,n));free(again);
        puts("PASS: a refused driver blob falls back for that member only, marks the pack stale and rewrites it once");

        /* 9. A member the pack cannot serve (its recipe bytes damaged, table checksums valid). */
        again=slurp(packp,&n2);
        u32 recipe_off=0;memcpy(&recipe_off,again+40+11*32+8,4);again[tables+recipe_off+64]^=1;
        fnv=pso_hash_bytes(1469598103934665603ull,again+tables,n-tables);memcpy(again+32,&fnv,8);
        spit(packp,again,n);free(again);
        load_race();CHECK(s_pack_hits==1 && resident_count(N)==N && !s_race_rejected && s_pack_stale==1);
        wait_released();session_end();
        puts("PASS: a pack member with a damaged recipe fails validation and is prepared from its own files");
        free(good);free(bad);

        /* 10. Menu-time warmup reads the race pack; the pack is released when it is done. */
        hide_files(1);
        pso_startup_begin();
        ULONGLONG began=GetTickCount64();
        while(resident_count(N)<N || s_startup_active){CHECK(GetTickCount64()-began<20000);Sleep(5);}
        CHECK(s_pack_hits==1 && s_pso_warm_loaded==N && !s_pso_warm_rejected && !s_pack_bytes);
        hide_files(0);
        /* The loading gate afterwards finds everything resident. */
        const LONG64 before=s_driver_pso_calls;
        load_race();CHECK(resident_count(N)==N && s_driver_pso_calls==before);wait_released();
        session_end();
        puts("PASS: background warmup prepares the race phase from the pack alone; the later gate makes no driver call");
    }

    /* 11. A demand job is served while helpers work; a second load replaces the first; shutdown mid-load returns. */
    _putenv_s("RSX_RACE_PREWARM_PACK","0");_putenv_s("RSX_RACE_PREWARM_THREADS","8");
    pso_race_notify("PitSceneRoot.gpb");
    HANDLE t=CreateThread(NULL,0,load_thread,NULL,0,NULL);CHECK(t);
    ID3D12PipelineState* demand=NULL;HRESULT hr;ULONGLONG began=GetTickCount64();
    while((hr=pso_queue_get(hashes[N],&descs[N],&demand))==E_PENDING){CHECK(GetTickCount64()-began<10000);Sleep(1);}
    CHECK(SUCCEEDED(hr) && demand);demand->lpVtbl->Release(demand);
    CHECK(WaitForSingleObject(t,35000)==WAIT_OBJECT_0);CloseHandle(t);CHECK(resident_count(N)==N);
    for(int round=0;round<20;round++){
        pso_warm_clear();pso_race_notify("PitSceneRoot.gpb");
        t=CreateThread(NULL,0,load_thread,NULL,0,NULL);CHECK(t);
        Sleep((DWORD)(round%4));pso_race_notify("PitSceneRoot.gpb");
        HANDLE t2=CreateThread(NULL,0,load_thread,NULL,0,NULL);CHECK(t2);
        CHECK(WaitForSingleObject(t,35000)==WAIT_OBJECT_0 && WaitForSingleObject(t2,35000)==WAIT_OBJECT_0);
        CloseHandle(t);CloseHandle(t2);
        began=GetTickCount64();while(resident_count(N)<N){CHECK(GetTickCount64()-began<10000);Sleep(1);}
    }
    for(int round=0;round<20;round++){
        pso_warm_clear();pso_race_notify("PitSceneRoot.gpb");
        t=CreateThread(NULL,0,load_thread,NULL,0,NULL);CHECK(t);
        Sleep((DWORD)(round%3));
        began=GetTickCount64();pso_queue_stop();CHECK(GetTickCount64()-began<5000);
        CHECK(WaitForSingleObject(t,5000)==WAIT_OBJECT_0);CloseHandle(t);
        CHECK(!s_race_waiters && !g_rsx_race_load_waiting && !s_prewarm_thr_n && !s_pack_bytes);
        s_pso_stopping=0;pso_race_notify("TopMenuTemplate.gpb");
    }
    session_end();
    puts("PASS: demand jobs complete beside eight helpers; 20 overlapping reloads all finish; 20 mid-load shutdowns return with no waiter or thread left");

    /* Leave a pack of the full set for the Python cross-check. */
    _putenv_s("RSX_RACE_PREWARM_PACK","");_putenv_s("RSX_RACE_PREWARM_THREADS","");
    DeleteFileA(packp);stored=s_pack_stored;
    load_race();wait_stored(stored);wait_released();session_end();
    CHECK(pack_count()==N);
    pso_queue_debug(stdout);
    puts("PASS: race pack and helper prewarm");
    return 0;
}
