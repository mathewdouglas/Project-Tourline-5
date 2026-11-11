/* CPU selection stays in a baseline TU; only private alternate images use
 * AVX/FMA. The generated manifest holds this target's audio identities and
 * optional profiled render bodies. Register at bootstrap, before any SPU
 * runs or caches a lookup. GT5P_AUDIO_NATIVE=0 restores portable audio;
 * GT5P_SPU_FMA=1 opts into the measured GT5 render body's FMA copy. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef GT5P_HAVE_AUDIO_NATIVE
#include <intrin.h>
#include "spu_workload.h"
#include "spu_interp.h"
extern "C" unsigned char* vm_base;
#define AUDIO_IMAGE(name, fingerprint, image) \
    extern "C" void gt5p_audio_##name##_spu_func_00000000(spu_context*); \
    extern "C" void gt5p_audio_##name##_spu_recomp_register(void);
#define SPU_IMAGE(name, fingerprint, image, ea, span) \
    extern "C" void gt5p_audio_##name##_spu_recomp_register(void);
#include "gt5p_audio_images.inc"
#undef SPU_IMAGE
#undef AUDIO_IMAGE

static __attribute__((target("xsave"), noinline)) uint64_t audio_xcr0() { return _xgetbv(0); }
static int registering_image;
static unsigned found, missing;
static bool installing;

extern "C" void gt5p_audio_register_function(uint32_t addr, spu_lifted_fn fn)
{
    if (spu_override_function(registering_image, addr, installing ? fn : nullptr)) ++found;
    else ++missing;
}
#endif

extern "C" void gt5p_audio_select_native(void)
{
#ifdef GT5P_HAVE_AUDIO_NATIVE
    const char* e = getenv("GT5P_AUDIO_NATIVE");
    const bool audio_on = !(e && *e == '0');
    const char* render = getenv("GT5P_SPU_FMA");
    const bool render_on = render && *render == '1';  /* opt in until broader rendering acceptance */
    if (!audio_on && !render_on) return;
    int cpu[4]; __cpuidex(cpu, 1, 0);
    constexpr unsigned required = (1u << 12) | (1u << 27) | (1u << 28);
    if (((unsigned)cpu[2] & required) != required || (audio_xcr0() & 6) != 6) {
        printf("[spu-native] FMA unavailable to CPU/OS; keeping portable images\n");
        return;
    }
    const struct {
        uint64_t fingerprint; int image; const char* name;
        spu_lifted_entry_fn entry; void (*register_all)(void);
        uint32_t ea, span;
    } images[] = {
#define AUDIO_IMAGE(name, fingerprint, image) \
        {fingerprint, image, #name, gt5p_audio_##name##_spu_func_00000000, gt5p_audio_##name##_spu_recomp_register, 0, 0},
#define SPU_IMAGE(name, fingerprint, image, ea, span) \
        {fingerprint, image, #name, nullptr, gt5p_audio_##name##_spu_recomp_register, ea, span},
#include "gt5p_audio_images.inc"
#undef SPU_IMAGE
#undef AUDIO_IMAGE
    };
    for (const auto& image : images) {
        int actual = 0;
        if (image.ea) {
            if (!render_on) continue;
            if (!vm_base || spu_workload_fingerprint(vm_base + image.ea, image.span) != image.fingerprint) {
                fprintf(stderr, "[spu-fma] %s content mismatch; keeping portable body\n", image.name);
                continue;
            }
            actual = image.image;
        } else {
            if (!audio_on || !spu_workload_find_img(image.fingerprint, &actual) || actual != image.image) continue;
        }
        registering_image = actual; installing = false; found = missing = 0;
        image.register_all();
        if (missing || !found) {
            fprintf(stderr, "[%s] %s registry mismatch; keeping portable image\n", image.ea ? "spu-fma" : "audio-native", image.name);
            continue;
        }
        installing = true; found = missing = 0;
        image.register_all();
        if (!image.ea) spu_workload_register_img(image.fingerprint, image.entry, actual, image.name);
        printf("[%s] %s: %u native functions, image %d\n", image.ea ? "spu-fma" : "audio-native", image.name, found, actual);
    }
#endif
}
