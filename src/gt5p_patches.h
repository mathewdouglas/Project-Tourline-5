#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Runtime-guarded PPU instruction patch switches (default 0 = vanilla).
 * Referenced directly by lifted PPU chunks when built with --insn-patches. */
extern int g_gt5_patch_looser_chase_camera;
extern int g_gt5_patch_disable_dithering;
extern int g_gt5_patch_enable_bloom_and_lens_flares_in_all_cameras;
extern int g_gt5_patch_chase_camera_car5_fov_and_pitch;
extern int g_gt5_patch_analogue_dials_in_all_cameras;
extern int g_gt5_patch_use_2x_msaa_in_720p_mode;
extern int g_gt5_patch_unlock_fps;

/* Call once after ELF load in main(). Reads settings + GT5_PATCH_* env vars
 * and writes any enabled boot-time data/BSS patches into guest memory. */
void gt5p_patches_apply_boot(void);

/* Live toggle and tuning for Looser Chase Camera (F1 > GAME or boot).
 * Writes the target's chase-camera BSS float vectors immediately; disabling
 * restores the vanilla values (damper 300, max angle 30). */
void gt5p_patches_apply_chase_cam(bool enabled, int damper, int max_angle);

/* True when the compiled target has a measured chase-camera BSS address. */
bool gt5p_patches_has_chase_cam(void);

#ifdef __cplusplus
}
#endif
