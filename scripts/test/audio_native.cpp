/* CPU dispatch tests use inert image entries, never AVX instructions. */
#include <assert.h>
#include <string.h>
#include <intrin.h>
static unsigned caps = (1u<<12)|(1u<<27)|(1u<<28), xreads;
static unsigned long long state = 6;
static void test_cpuid(int* out, int, int) { memset(out,0,16); out[2]=(int)caps; }
static unsigned long long test_xgetbv(unsigned) { ++xreads; return state; }
#define __cpuidex test_cpuid
#define _xgetbv test_xgetbv
#include "../../src/gt5p_audio_native.cpp"
static int swaps, overrides, probes, bad_id;
static bool omit, absent, wrong_body, omit_body;
struct TestImage { uint64_t fp; int id; };
static constexpr TestImage test_images[] = {
#define AUDIO_IMAGE(name, fp, id) {fp, id},
#define SPU_IMAGE(name, fp, id, ea, span)
#include "gt5p_audio_images.inc"
#undef AUDIO_IMAGE
#undef SPU_IMAGE
};
static constexpr int count = sizeof(test_images) / sizeof(test_images[0]);
static constexpr int second = test_images[1].id;
static unsigned char guest_memory[0x1980000];
extern "C" unsigned char* vm_base = guest_memory;
extern "C" uint64_t spu_workload_fingerprint(const void*, size_t) {
    return wrong_body ? 0 : 0x76FF30204F3B9A9EULL;
}
extern "C" int spu_override_function(int image, uint32_t addr, spu_lifted_fn fn) {
    if(((image==second && omit) || (image==51 && omit_body)) && addr==0x100)return 0;
    if(fn)++overrides;else ++probes;
    return 1;
}
static void stock(spu_context*) {}
extern "C" spu_lifted_entry_fn spu_workload_find_img(uint64_t fp,int* id) {
    for (const auto& i : test_images) if(i.fp==fp) {
        *id=i.id;
        if(i.id==second) {
            if(absent)return nullptr;
            if(bad_id)*id=999;
        }
        return stock;
    }
    assert(!"foreign fingerprint"); return nullptr;
}
extern "C" void spu_workload_register_img(uint64_t,spu_lifted_entry_fn,int,const char*){++swaps;}
#define STUB(name) \
 extern "C" void gt5p_audio_##name##_spu_func_00000000(spu_context*){} \
 extern "C" void gt5p_audio_##name##_spu_recomp_register(void){ \
 gt5p_audio_register_function(0,gt5p_audio_##name##_spu_func_00000000); \
 gt5p_audio_register_function(0x100,gt5p_audio_##name##_spu_func_00000000);}
#define AUDIO_IMAGE(name, fp, id) STUB(name)
#define SPU_IMAGE(name, fp, id, ea, span) STUB(name)
#include "gt5p_audio_images.inc"
#undef AUDIO_IMAGE
#undef SPU_IMAGE
int main(int argc,char** argv){
 assert(argc==2);const char* mode=argv[1];
 if(!strcmp(mode,"cpu"))caps=0;
 if(!strcmp(mode,"os"))state=2;
 if(!strcmp(mode,"missing"))omit=true;
 if(!strcmp(mode,"identity"))bad_id=1;
 if(!strcmp(mode,"absent"))absent=true;
 wrong_body=!strcmp(mode,"body_mismatch");
 omit_body=!strcmp(mode,"body_missing");
 gt5p_audio_select_native();
 bool disabled=!strcmp(mode,"off")||!strcmp(mode,"cpu")||!strcmp(mode,"os");
 assert(swaps==(disabled||!strcmp(mode,"render_only")?0:count-(omit||bad_id||absent?1:0)));
 int body = 0;
#ifdef TEST_GT5
 if(getenv("GT5P_SPU_FMA") && *getenv("GT5P_SPU_FMA")=='1' && !disabled && !wrong_body && !omit_body)body=1;
#endif
 assert(overrides==2*(swaps+body));
 if(!strcmp(mode,"cpu")||!strcmp(mode,"off"))assert(xreads==0);
 if(omit)assert(probes==2*count-1); /* The second image was validated, never partially installed. */
 puts(mode);
}
