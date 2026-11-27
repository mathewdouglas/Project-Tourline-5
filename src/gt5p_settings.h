/* Player settings, read from gt5p_settings.toml at start-up.
 *
 * The file lives beside the game: the first gt5p_settings.toml found in the
 * exe's folder or the folders above it (where input\EMAIN.ELF is), else the
 * working directory. If there is none, one is written with the defaults and
 * a comment on every key, so there is always a file to edit.
 *
 * Most settings are switches the runtime already reads from the environment
 * (RSX_MSAA, PAD_NO_SDL, ...). Loading sets those variables, but only when
 * they are not set already: a variable set by hand or by a script still wins
 * over the file. The rest are read through gt5p_settings().
 *
 *   GT5P_SETTINGS=<path>  read this file instead (written if missing)
 *   GT5P_NO_SETTINGS=1    ignore the file; built-in defaults, nothing applied.
 *                         For measurement runs that must not depend on it.
 */
#pragma once

#include <string>

struct Gt5pSettings {
    /* [video] */
    int resolution = 720;       /* 720, 1080, 1440 or 2160 (above 1080 the game is told 1080p) */
    int msaa       = 4;         /* samples for targets the game antialiases: 1 (off), 2, 4, 8 */
    int dlss_mode  = 0;         /* 0 off, 1 dlaa, 2 quality, 3 balanced, 4 performance, 5 ultra (RSX_AA) */
    int frame_gen  = 0;         /* 0 off, 1 2x, 2 3x, 3 4x (RSX_FG) */
    bool ultrawide = false;     /* 21:9 picture (GT5P_ULTRAWIDE, docs/plan-ultrawide.md) */
    bool borderless = false;    /* a borderless window over the monitor (RSX_WINDOW_MODE) */
    bool vsync        = true;   /* wait for the monitor's vblank when presenting */
    bool shader_cache = true;   /* keep compiled shaders in shadercache/ between runs */
    bool shader_dxc = true;     /* guest SM 6.0 (default since 0.1.0); "fxc" selects SM 5.0 */
    bool shader_indicator = true;   /* "Compiling shaders" over the game while a draw waits on a shader */
    bool preload_indicator = false; /* the quieter "Loading shaders" badge for background preload work */
    bool shader_warmup = true;      /* prepare recorded pipelines during startup in the background */
    bool shader_warmup_startup = false; /* ...or before the game starts, on all cores (Phase 44) */
    bool pipeline_async = true;    /* build driver pipelines on one background worker */
    bool race_preload = true;      /* prepare learned race pipelines during loading */
    bool shader_interp = true;     /* interpreter shaders draw while a shader compiles (RSX_SHADER_INTERP) */
    bool shader_precompile = true;  /* compile new shaders on background threads, all at once */

    /* [input] */
    bool sdl_controllers = true;    /* PlayStation / Switch / generic pads via SDL2.dll */
    bool keyboard        = true;    /* keyboard as pad 1 when no controller is connected */
    bool modern_controls = false;  /* last requested mapping, not a boot override */
    int trigger_deadzone = 0;       /* 0..20 percent; rescale the remaining travel */
    int trigger_curve = 0;          /* 0 linear, 1 gentle, 2 quick */
    bool adaptive_triggers = false; /* fixed DualSense resistance, experimental hardware */
    bool trigger_rumble = false;    /* supported Xbox triggers: existing game motor signals */
    bool sdl_rumble = false;        /* forward game body motors through dynamic SDL2 */
    int right_stick_dpad = 0;       /* 0 off, 1 in_race (RA), 2 always (PAD_RS_DPAD, #90) */
    int speed_hotkey_pad = 0;       /* controller button bitmask (CELL_PAD_CTRL_*) for speed toggle, 0 off (#90) */
    int speed_hotkey_key = 0x71;    /* keyboard VK for speed toggle (default VK_F2 = 0x71; 0 off, #90) */
    int button_prompts   = 0;       /* 0 auto, 1 PlayStation, 2 Xbox, 3 Nintendo, 4 keyboard (gt5p_glyphs.h) */

    /* [keyboard]: the key (Windows virtual-key code, 0 none) for each button,
     * in gt5p_keymap.h's order: Cross Circle Square Triangle L1 R1 L2 R2 L3
     * R3 Start Select Up Down Left Right. */
    int keys[16] = { 'Z', 'X', 'A', 'S', 'Q', 'W', '1', '2', 0, 0,
                     0x0D /* Enter */, 0x09 /* Tab */, 0x26, 0x28, 0x25, 0x27 /* arrows */ };

    /* [wheel]: a steering wheel shown to the game as a Logitech G27
     * (ps3recomp libs/hardware/wheel_*.c, docs/wheels.md). */
    bool wheel = true;          /* look for a wheel (PS3_WHEEL=off: never) */
    int wheel_range = 900;      /* degrees the PC wheel turns, lock to lock (PS3_WHEEL_RANGE) */
    int ffb_strength = 80;      /* force feedback strength, 0..100 % (PS3_WHEEL_FFB) */
    bool ffb_invert = false;    /* the game's steering force the other way (PS3_WHEEL_FFB_INVERT) */
    int ffb_springs = 100;      /* spring, damper and friction effects, 0..200 % (PS3_WHEEL_FFB_SPRING) */
    int ffb_min_force = 0;      /* minimum steering force, 0..50 % (PS3_WHEEL_FFB_MIN) */
    bool ffb_pause = true;      /* no forces while F1 is open or the game is in the background (PS3_WHEEL_FFB_MUTE) */
    std::string wheel_preset = "auto"; /* auto, g29, g920, g27, thrustmaster, custom */
    std::string wheel_steer;           /* "" (auto=X) or axis name X..S1 (PS3_WHEEL_STEER) */
    std::string wheel_pedals;          /* "" (auto) or "throttle=Y,brake=RZ,clutch=Z" (PS3_WHEEL_PEDALS) */
    std::string wheel_buttons;         /* "" (auto) or "cross=1,circle=3,..." (PS3_WHEEL_BUTTONS) */

    /* [game] */
    bool skip_movie = false;    /* press through the opening movie (PAD_SKIP_MOVIE) */
    bool smooth_colour = true;  /* 16-bit targets, no dither noise (RSX_HIGH_PRECISION) */
    int anisotropy = 16;        /* 1 (trilinear), 4, 8 or 16: forced anisotropic filtering (RSX_ANISO) */
    bool looser_chase_camera = false; /* RPCS3 Looser Chase Camera patch (GT5_PATCH_LOOSER_CHASE_CAMERA) */
    int chase_cam_damper = 90;        /* chase camera spring damper, 0..300 (vanilla 300) */
    int chase_cam_max_angle = 35;     /* chase camera max rotation angle, 0..100 (vanilla 30) */
    bool unlock_fps = false;          /* RPCS3 Unlock FPS patch (GT5_PATCH_UNLOCK_FPS, 02.11) */
    bool throttle_linearity = false;  /* RPCS3 Accurate Throttle Linearity patch (GT5_PATCH_ACCURATE_THROTTLE_LINEARITY) */
    std::string player_name;    /* the PS3 user name the game shows; "" = "User" (PS3_USER_NAME) */

    /* [audio] */
    int audio_channels = 2;     /* sound output: 2 stereo, 6 (5.1), 8 (7.1), 0 auto (AUDIO_OUTPUT_CHANNELS) */

    /* [performance] */
    int vblank_hz100 = 5994;    /* the vblank clock, in 1/100 Hz: 5994 (console) or 6250 */
    int lane_cores   = 2;       /* SPU render lanes on cores of their own: 0 off, 1 hint, 2 strict */
    bool fifth_spu_lane = false;    /* let the render lists use SPU 3 too (experimental) */

    /* [debug] */
    int verbose_log = -1;       /* -1 auto (quiet), 0 off, 1 on */
    bool debug_export = false;  /* opt in to bounded recent logs and F3 reports */

};

/* The settings in effect, environment overrides included. Built-in defaults
 * until gt5p_settings_load(). */
const Gt5pSettings& gt5p_settings();

/* The file loaded or created, or "" when none was used. */
const std::string& gt5p_settings_path();

/* Find, parse and apply the file. Call first thing in main(), before anything
 * reads the environment or starts a thread. Never fails: a missing key keeps
 * its default, and a bad file or value is reported on stderr and skipped. */
void gt5p_settings_load();

/* Write `s` to `path` in the commented layout. Returns false on I/O failure.
 * For a settings screen to persist changes; load uses it for the default file. */
bool gt5p_settings_save(const Gt5pSettings& s, const std::string& path);

/* For the in-game settings page (F1, gt5p_settings_ui.cpp). */

/* The values as the file has them, before environment overrides: what the
 * page edits and saves. Built-in defaults when there is no file. */
const Gt5pSettings& gt5p_settings_file();

/* True when `env_name` (RSX_MSAA, PAD_NO_SDL, GT5P_RESOLUTION, ...) was
 * already set when the file was loaded, so it overrides the file's value. */
bool gt5p_settings_overridden(const char* env_name);

/* False under GT5P_NO_SETTINGS: there is no file to save to. */
bool gt5p_settings_enabled();

/* A usable game.player_name: 1-16 letters, digits, spaces or . - _ ' (F1 > GAME > Player name). */
bool gt5p_player_name_valid(const std::string& v);

/* Save `s` as the file's values (and remember them as such). */
bool gt5p_settings_store(const Gt5pSettings& s);
