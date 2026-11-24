/* Runtime-guarded game patches (RPCS3 patch.yml equivalents).
 *
 * Each instruction patch is compiled into the lifted PPU code behind a global
 * `int g_gt5_patch_*` guard (default 0 = vanilla, zero overhead when off).
 * Boot-time and live data/BSS writes (such as Looser Chase Camera's spring
 * damper and max angle floats) live here so players can toggle them via
 * gt5p_settings.toml, the F1 menu, or GT5_PATCH_* environment variables.
 */
#include "gt5p_patches.h"
#include "gt5p_settings.h"
#include "ppu_context.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef GT5P_CHASE_CAM_BSS
#define GT5P_CHASE_CAM_BSS 0u
#endif
#ifndef GT5P_CHASE_CAM_V211
#define GT5P_CHASE_CAM_V211 0
#endif

extern "C" {
extern uint8_t* vm_base;

int g_gt5_patch_looser_chase_camera = 0;
int g_gt5_patch_disable_dithering = 0;
int g_gt5_patch_enable_bloom_and_lens_flares_in_all_cameras = 0;
int g_gt5_patch_chase_camera_car5_fov_and_pitch = 0;
int g_gt5_patch_analogue_dials_in_all_cameras = 0;
int g_gt5_patch_use_2x_msaa_in_720p_mode = 0;
int g_gt5_patch_unlock_fps = 0;
int g_gt5_patch_accurate_throttle_linearity = 0;
}

static void write_be_f32(uint32_t ea, float val)
{
    if (!vm_base || !ea) return;
    uint32_t u = 0;
    memcpy(&u, &val, 4);
    u = ((u & 0x000000FFu) << 24) | ((u & 0x0000FF00u) << 8) |
        ((u & 0x00FF0000u) >> 8)  | ((u & 0xFF000000u) >> 24);
    memcpy(vm_base + ea, &u, 4);
}

static int env_flag(const char* name, int def)
{
    const char* e = getenv(name);
    if (!e || !e[0]) return def;
    return (e[0] != '0') ? 1 : 0;
}

extern "C" bool gt5p_patches_has_chase_cam(void)
{
    return GT5P_CHASE_CAM_BSS != 0u;
}

extern "C" bool gt5p_patches_has_unlock_fps(void)
{
    return GT5P_CHASE_CAM_V211 != 0;
}

extern "C" void gt5p_patches_apply_unlock_fps(bool enabled)
{
    if (!gt5p_patches_has_unlock_fps()) {
        g_gt5_patch_unlock_fps = 0;
        return;
    }
    g_gt5_patch_unlock_fps = enabled ? 1 : 0;
}

extern "C" bool gt5p_patches_has_throttle_linearity(void)
{
    return GT5P_CHASE_CAM_BSS != 0u;
}

extern "C" void gt5p_patches_apply_throttle_linearity(bool enabled)
{
    if (!gt5p_patches_has_throttle_linearity()) {
        g_gt5_patch_accurate_throttle_linearity = 0;
        return;
    }
    g_gt5_patch_accurate_throttle_linearity = enabled ? 1 : 0;
}

/* RPCS3 "Accurate Throttle Linearity" (Pappapatu, 01.04 0x00551510 / 02.11 0x0048D0DC):
 * Inverts GT5's piecewise-linear throttle LUT for action ID 4 (r30 == 4) so
 * throttle response is 1:1 with input without altering brake, steering, or
 * wheel-pedal inputs. Runs immediately before `extsw r7, r3`. */
extern "C" void gt5p_patch_throttle_linearity_hook(ppu_context* ctx)
{
    if ((uint32_t)ctx->gpr[30] != 4u)
        return;
    uint32_t v = (uint32_t)ctx->gpr[3];
    if (v < 0x2000u)
        v = v * 3u;
    else
        v = (v - 0x2000u) / 3u + 0x6000u;
    ctx->gpr[3] = (uint64_t)v;
}

extern "C" void gt5p_patches_apply_chase_cam(bool enabled, int damper, int max_angle)
{
    if (!GT5P_CHASE_CAM_BSS || !vm_base) {
        g_gt5_patch_looser_chase_camera = 0;
        return;
    }
    if (damper < 0) damper = 0;
    if (damper > 300) damper = 300;
    if (max_angle < 0) max_angle = 0;
    if (max_angle > 100) max_angle = 100;

    g_gt5_patch_looser_chase_camera = enabled ? 1 : 0;
    const uint32_t base = (uint32_t)GT5P_CHASE_CAM_BSS;
    const float f_damper = enabled ? (float)damper : 300.0f;
    const float f_angle  = enabled ? (float)max_angle : 30.0f;

#if GT5P_CHASE_CAM_V211
    /* 02.11 BSS layout at 0x017FDF40:
     *   +0x00: (30.0, damper, 200.0, 0.0)
     *   +0x10: (10.0, max_angle, 20.0, 0.0) */
    write_be_f32(base + 0x00u, 30.0f);
    write_be_f32(base + 0x04u, f_damper);
    write_be_f32(base + 0x08u, 200.0f);
    write_be_f32(base + 0x0Cu, 0.0f);
    write_be_f32(base + 0x10u, 10.0f);
    write_be_f32(base + 0x14u, f_angle);
    write_be_f32(base + 0x18u, 20.0f);
    write_be_f32(base + 0x1Cu, 0.0f);
#else
    /* 01.04 BSS layout at 0x019AFE70:
     *   +0x00: (20.0, damper, 0.0, 0.0)
     *   +0x10: (10.0, max_angle, 0.0, 0.0) */
    write_be_f32(base + 0x00u, 20.0f);
    write_be_f32(base + 0x04u, f_damper);
    write_be_f32(base + 0x08u, 0.0f);
    write_be_f32(base + 0x0Cu, 0.0f);
    write_be_f32(base + 0x10u, 10.0f);
    write_be_f32(base + 0x14u, f_angle);
    write_be_f32(base + 0x18u, 0.0f);
    write_be_f32(base + 0x1Cu, 0.0f);
#endif
}

extern "C" void gt5p_patches_apply_boot(void)
{
    const Gt5pSettings& s = gt5p_settings();
    if (GT5P_CHASE_CAM_BSS) {
        const bool on = env_flag("GT5_PATCH_LOOSER_CHASE_CAMERA", s.looser_chase_camera ? 1 : 0) != 0;
        if (on) {
            gt5p_patches_apply_chase_cam(true, s.chase_cam_damper, s.chase_cam_max_angle);
            printf("[GT5P] patch: Looser Chase Camera enabled (damper=%d, max_angle=%d, bss=0x%08X)\n",
                   s.chase_cam_damper, s.chase_cam_max_angle, (unsigned)GT5P_CHASE_CAM_BSS);
        }
    }

    if (gt5p_patches_has_unlock_fps()) {
        const bool on = env_flag("GT5_PATCH_UNLOCK_FPS", s.unlock_fps ? 1 : 0) != 0;
        if (on) {
            gt5p_patches_apply_unlock_fps(true);
            printf("[GT5P] patch: Unlock FPS enabled\n");
        }
    }

    if (gt5p_patches_has_throttle_linearity()) {
        const bool on = env_flag("GT5_PATCH_ACCURATE_THROTTLE_LINEARITY", s.throttle_linearity ? 1 : 0) != 0;
        if (on) {
            gt5p_patches_apply_throttle_linearity(true);
            printf("[GT5P] patch: Accurate Throttle Linearity enabled\n");
        }
    }

    struct EnvPatch { int* guard; const char* env; const char* title; };
    const EnvPatch extra[] = {
        { &g_gt5_patch_disable_dithering, "GT5_PATCH_DISABLE_DITHERING", "Disable Dithering" },
        { &g_gt5_patch_enable_bloom_and_lens_flares_in_all_cameras, "GT5_PATCH_ENABLE_BLOOM_AND_LENS_FLARES_IN_ALL_CAMERAS", "Enable Bloom and Lens Flares in All Cameras" },
        { &g_gt5_patch_chase_camera_car5_fov_and_pitch, "GT5_PATCH_CHASE_CAMERA_CAR5_FOV_AND_PITCH", "Chase Camera - CAR5 FOV and Pitch" },
        { &g_gt5_patch_analogue_dials_in_all_cameras, "GT5_PATCH_ANALOGUE_DIALS_IN_ALL_CAMERAS", "Analogue Dials in All Cameras" },
        { &g_gt5_patch_use_2x_msaa_in_720p_mode, "GT5_PATCH_USE_2X_MSAA_IN_720P_MODE", "Use 2x MSAA in 720p Mode" },
    };
    for (const auto& p : extra) {
        if (env_flag(p.env, 0)) {
            *p.guard = 1;
            printf("[GT5P] patch: %s enabled (%s=1)\n", p.title, p.env);
        }
    }
}
