/* In-game settings page: F1 (or the PS button on a controller) over the game.
 *
 * Shows every setting of gt5p_settings.toml, changes it, and saves the file
 * at once. Settings the runtime can change while running (vsync, the keyboard
 * as a controller) take effect immediately; the rest say "after restart", and
 * a "Restart now" row relaunches the game with the new file.
 *
 * The page is drawn into the game's own picture: it is painted with GDI into
 * a bitmap at the backbuffer's scale (layout units are 1280 x 720 pixels),
 * and the D3D12 backend blends that over each frame before present
 * (rsx_d3d12_overlay_set). So it shows in fullscreen and in captures, and
 * keyboard and mouse are read while the game window has the focus.
 * It is styled after the game's own screens (Options, its Confirmation
 * popup). Escape in the game window asks "Quit Gran Turismo 5?" in
 * a copy of that popup instead of closing at once (GT5P_ESC_CONFIRM=0: close
 * at once; GT5P_QUIT_UI=1 asks at start, for tests).
 * GT5P_SETTINGS_UI_LEGACY=1 (or RSX_OVERLAY=0) keeps the older form: a GDI
 * window of its own over the game window, like the F3 profiler.
 * While it is open the game sees every controller at rest
 * (g_pad_input_blocked, cellPad.c), and the page reads the controllers itself.
 *
 *   keyboard    Up/Down select, Left/Right change, Enter/Space change or
 *               press, Esc/F1/Backspace close (Escape does not close the
 *               game while the page is up)
 *   controller  D-pad, Cross change or press, Circle close, PS opens/closes
 *   mouse       click a row to select it, click its value to change it
 *
 * GT5P_SETTINGS_UI=0: no page; =1: open it as soon as the window is up (for
 * testing). Under GT5P_NO_SETTINGS the page opens but
 * cannot save.
 *
 * To add a setting: a field in Gt5pSettings, its key in gt5p_settings.cpp
 * (read, known_key, save), and a row in items() below.
 */
#include <windows.h>
#include <windowsx.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <wctype.h>

#include <string>
#include <vector>

#include "gt5p_settings.h"
#include "gt5p_patches.h"
#include "gt5p_controls.h"
#include "gt5p_debug_export.h"
#include "gt5p_glyphs.h"
#include "gt5p_keymap.h"
#include "pt5_restart.h"

#pragma comment(lib, "msimg32.lib")   /* AlphaBlend */

extern "C" {
extern volatile int g_pad_input_blocked;        /* cellPad.c */
extern volatile uint32_t g_pad_menu_buttons;    /* cellPad.c: bit 16 = PS button */
extern volatile uint32_t g_pad_inject_buttons;  /* cellPad.c: pressed for the title on port 0 */
void pad_set_keyboard_enabled(int on);          /* cellPad.c */
struct WheelRawUi {
    int connected;
    char name[128];
    uint16_t vid, pid;
    int ffb;
    float axis[8];
    uint32_t buttons;
    int pov;
    char hint[128];
    uint32_t buttons_ext[7];
    int aux_count;
    char aux_name[64];
};
int  wheel_host_raw(WheelRawUi* out);           /* wheel_host_win.c */
void wheel_host_set_ffb(int gain, int invert, int spring, int min_force);   /* wheel_host_win.c */
void wheel_host_set_map(const char* steer, const char* pedals, const char* buttons); /* wheel_host_win.c */
void rsx_d3d12_set_present_sync(int interval);  /* rsx_d3d12_backend.c */
void gt5p_shader_indicator_enable(int on);      /* gt5p_shaderbusy.cpp */
void gt5p_preload_indicator_enable(int on);
extern void (*g_pad_screen_hook)(const char*);  /* cellPad.c: every file the title opens */
int  rsx_d3d12_overlay_available(void);         /* rsx_d3d12_backend.c */
void rsx_d3d12_backbuffer_size(int* w, int* h);
void rsx_d3d12_output_rect(int* x, int* y, int* w, int* h);
void rsx_d3d12_overlay_set(const void* bgra, int w, int h, int x, int y);
extern int (*rsx_d3d12_on_escape)(void);      /* Escape in the game window */
}

namespace {

/* CELL_PAD_CTRL_* (cellPad.h) */
const uint32_t kSelect = 1u << 0, kStart = 1u << 3, kUp = 1u << 4, kRight = 1u << 5, kDown = 1u << 6, kLeft = 1u << 7,
               kCircle = 1u << 13, kCross = 1u << 14, kPs = 1u << 16,
               kL1 = 1u << 10, kR1 = 1u << 11;

struct Choice { int value; const wchar_t* label; };

// Version selection is UI state; it never enters the PC-settings TOML or
// changes the runtime settings layout used by other translation units.
struct UiSettings : Gt5pSettings {
    int launcher_version = 104;
    int launcher_mod = 0;      /* index into GT5P_MOD_CHOICES, 0 = Off */
    UiSettings& operator=(const Gt5pSettings& value) { Gt5pSettings::operator=(value); return *this; }
};
struct Item {
    const wchar_t* section;
    const wchar_t* label;
    const wchar_t* help;
    const char* env;                         /* variable that overrides it */
    std::vector<Choice> choices;
    int  (*get)(const UiSettings&);
    void (*set)(UiSettings&, int);
    void (*apply)(int);                      /* live; nullptr: after a restart */
    int key = -1;                            /* a keyboard binding: its button (gt5p_keymap.h) */
    int wmap = -1;                           /* a wheel mapping slot (0..3 axes, 4..15 buttons) */
};

/* Whether an item is overridden by variable `name`. Some items have no
 * variable (env is nullptr): never pass it to strcmp (issue #17, the DEBUG
 * page crashed on open in 0.1.6). */
bool env_is(const Item& it, const char* name) { return it.env && !strcmp(it.env, name); }

/* Items the launcher owns: the game version and the mod. Never in the TOML;
 * a change restarts through the launcher (pt5_restart.h). */
bool launcher_item(const Item& it) { return env_is(it, "GT5P_GAME_VERSION") || env_is(it, "GT5P_MOD"); }

/* F1 > Game > Mod: the launcher's choices (GT5P_MOD_CHOICES, '|' between
 * them, Off first; launcher/mods.h). Empty when not started by the launcher
 * or with PT5_MODS=0. */
const std::vector<Choice>& mod_choices()
{
    static std::vector<std::wstring> labels;
    static std::vector<Choice> v;
    static bool done = false;
    if (!done) {
        done = true;
        const char* e = getenv("GT5P_MOD_CHOICES");
        std::string all = e ? e : "";
        for (size_t at = 0; !all.empty() && at <= all.size();) {
            size_t bar = all.find('|', at);
            if (bar == std::string::npos) bar = all.size();
            const std::string one = all.substr(at, bar - at);
            int n = MultiByteToWideChar(CP_ACP, 0, one.c_str(), (int)one.size(), nullptr, 0);
            std::wstring w(n, L'\0');
            if (n) MultiByteToWideChar(CP_ACP, 0, one.c_str(), (int)one.size(), &w[0], n);
            labels.push_back(w);
            at = bar + 1;
        }
        for (size_t i = 0; i < labels.size(); i++) v.push_back({ (int)i, labels[i].c_str() });
    }
    return v;
}
bool mods_available() { return pt5_restart::managed() && !mod_choices().empty(); }

const std::vector<Choice> kOnOff = { {1, L"On"}, {0, L"Off"} };

/* Keyboard bindings: one item per button, the value a virtual-key code. */
template <int N> int key_get(const UiSettings& s) { return s.keys[N]; }
template <int N> void key_set(UiSettings& s, int v) { s.keys[N] = v; }
template <int N> void key_apply(int v) { gt5p_keymap_apply_one(N, v); }
const wchar_t* const kKeyHelp =
    L"Cross or Enter, then press the key for this button. Escape cancels. A key another "
    L"button has moves to it. Used when no controller is connected.";
#define KEY_ITEM(n, label) { L"KEYBOARD", label, kKeyHelp, "PAD_KEYMAP", {}, key_get<n>, key_set<n>, key_apply<n>, n }

/* The wheel's force-feedback rows apply live: each sends all four values, the
 * others as last sent (from the settings in effect at first). */
enum { kFfbGain, kFfbInvert, kFfbSpring, kFfbMin };
void wheel_ffb_apply(int which, int v)
{
    static int cur[4] = { -1 };
    if (cur[0] < 0) {
        const Gt5pSettings& s = gt5p_settings();
        cur[kFfbGain] = s.ffb_strength; cur[kFfbInvert] = s.ffb_invert ? 1 : 0;
        cur[kFfbSpring] = s.ffb_springs; cur[kFfbMin] = s.ffb_min_force;
    }
    cur[which] = v;
    wheel_host_set_ffb(cur[kFfbGain], cur[kFfbInvert], cur[kFfbSpring], cur[kFfbMin]);
}

/* Wheel presets and interactive axis/button mapping (F1 > WHEEL & WHEEL MAP). */
enum { kPresetAuto = 0, kPresetG29, kPresetG920, kPresetG27, kPresetThrustmaster, kPresetCustom };
static const char* const kWheelAxisNames[8] = { "X", "Y", "Z", "RX", "RY", "RZ", "S0", "S1" };
static const char* const kWheelPedalKeys[3] = { "throttle", "brake", "clutch" };
/* 16 G27 buttons (including button-based D-pad for wheels without a POV hat)
 * exposed on the 2-column WHEEL MAP page (slots 4..19). */
struct WheelBtnSlot { const char* key; const wchar_t* label; };
static const WheelBtnSlot kWheelBtnSlots[16] = {
    { "r1",       L"Shift Up (R1)" },
    { "l1",       L"Shift Down (L1)" },
    { "cross",    L"Cross" },
    { "circle",   L"Circle" },
    { "square",   L"Square" },
    { "triangle", L"Triangle" },
    { "l2",       L"L2" },
    { "r2",       L"R2" },
    { "l3",       L"L3" },
    { "r3",       L"R3" },
    { "start",    L"Start" },
    { "select",   L"Select" },
    { "up",       L"D-Pad Up" },
    { "down",     L"D-Pad Down" },
    { "left",     L"D-Pad Left" },
    { "right",    L"D-Pad Right" },
};

int wheel_preset_id(const std::string& p)
{
    if (p == "g29") return kPresetG29;
    if (p == "g920") return kPresetG920;
    if (p == "g27") return kPresetG27;
    if (p == "thrustmaster") return kPresetThrustmaster;
    if (p == "custom") return kPresetCustom;
    return kPresetAuto;
}

void wheel_apply_preset_values(UiSettings& s, int p)
{
    switch (p) {
    case kPresetG29:
        s.wheel_preset = "g29";
        s.wheel_steer = "X";
        s.wheel_pedals = "throttle=Y,brake=RZ,clutch=S0";
        s.wheel_buttons = "cross=1,square=2,circle=3,triangle=4,r1=5,l1=6,r2=7,l2=8,select=9,start=10,r3=11,l3=12";
        break;
    case kPresetG920:
        s.wheel_preset = "g920";
        s.wheel_steer = "X";
        s.wheel_pedals = "throttle=Y,brake=Z,clutch=RZ";
        s.wheel_buttons = "cross=1,circle=2,square=3,triangle=4,r1=5,l1=6,start=7,select=8,r3=9,l3=10,r2=0,l2=0";
        break;
    case kPresetG27:
        s.wheel_preset = "g27";
        s.wheel_steer = "X";
        s.wheel_pedals = "throttle=Y,brake=Z,clutch=RZ";
        s.wheel_buttons = "cross=1,square=2,circle=3,triangle=4,r1=5,l1=6,r2=7,l2=8,select=9,start=10,r3=11,l3=12";
        break;
    case kPresetThrustmaster:
        s.wheel_preset = "thrustmaster";
        s.wheel_steer = "X";
        s.wheel_pedals = "throttle=RZ,brake=Y,clutch=S0";
        s.wheel_buttons = "";
        break;
    case kPresetCustom:
        s.wheel_preset = "custom";
        break;
    default:
        s.wheel_preset = "auto";
        s.wheel_steer = "";
        s.wheel_pedals = "";
        s.wheel_buttons = "";
        break;
    }
}

/* Parse/format key=val pairs in a comma-separated string (case-insensitive key). */
std::string wheel_kv_get(const std::string& csv, const char* key)
{
    const size_t klen = strlen(key);
    for (size_t i = 0; i < csv.size();) {
        size_t j = csv.find(',', i);
        if (j == std::string::npos) j = csv.size();
        const std::string tok = csv.substr(i, j - i);
        const size_t eq = tok.find('=');
        if (eq == klen && _strnicmp(tok.c_str(), key, klen) == 0)
            return tok.substr(eq + 1);
        i = j + 1;
    }
    return "";
}

std::string wheel_kv_set(const std::string& csv, const char* key, const std::string& val)
{
    const size_t klen = strlen(key);
    std::string out;
    bool replaced = false;
    for (size_t i = 0; i < csv.size();) {
        size_t j = csv.find(',', i);
        if (j == std::string::npos) j = csv.size();
        const std::string tok = csv.substr(i, j - i);
        const size_t eq = tok.find('=');
        if (eq == klen && _strnicmp(tok.c_str(), key, klen) == 0) {
            if (!val.empty()) {
                if (!out.empty()) out += ',';
                out += std::string(key) + "=" + val;
            }
            replaced = true;
        } else if (!tok.empty()) {
            if (!out.empty()) out += ',';
            out += tok;
        }
        i = j + 1;
    }
    if (!replaced && !val.empty()) {
        if (!out.empty()) out += ',';
        out += std::string(key) + "=" + val;
    }
    return out;
}

const wchar_t* const kWheelAxisHelp =
    L"Enter / Click: turn wheel or press pedal to bind.  I: invert pedal axis.  Delete: clear / auto.";
const wchar_t* const kWheelBtnHelp =
    L"Enter / Click: press a button on your wheel to bind it (applies live).  Delete: clear.";
#define WMAP_AXIS(slot, label) { L"WHEEL MAP", label, kWheelAxisHelp, (slot) == 0 ? "PS3_WHEEL_STEER" : "PS3_WHEEL_PEDALS", {}, \
    [](const UiSettings&) { return 0; }, [](UiSettings&, int) {}, [](int) {}, -1, slot }
#define WMAP_BTN(slot, label) { L"WHEEL MAP", label, kWheelBtnHelp, "PS3_WHEEL_BUTTONS", {}, \
    [](const UiSettings&) { return 0; }, [](UiSettings&, int) {}, [](int) {}, -1, slot }

const std::vector<Item>& items()
{
    static const std::vector<Item> v = {
        { L"VIDEO", L"Resolution",
          L"The display mode the game is told it has, and the window size. The game renders "
          L"natively at it; at 21:9, 1440p is 3440 x 1440 and 4K 5120 x 2160. The view is the same at every "
          L"resolution, only sharper.",
          "GT5P_RESOLUTION", { {720, L"1280 x 720"}, {1080, L"1920 x 1080"}, {1440, L"2560 x 1440"}, {2160, L"3840 x 2160 (4K)"} },
          [](const UiSettings& s) { return s.resolution; },
          [](UiSettings& s, int v) { s.resolution = v; }, nullptr },
        { L"VIDEO", L"Aspect ratio",
          L"21:9 makes the window ultrawide (2560x1080 at 1080p, 3440x1440 at 1440p): races and the dealer show a "
          L"wider view, menus stay centred at 16:9. Best with 1080p. Takes effect after a restart.",
          "GT5P_ULTRAWIDE", { {0, L"16:9"}, {1, L"21:9 ultrawide"} },
          [](const UiSettings& s) { return s.ultrawide ? 1 : 0; },
          [](UiSettings& s, int v) { s.ultrawide = v != 0; }, nullptr },
        { L"VIDEO", L"Window",
          L"Borderless fullscreen covers the whole monitor with a borderless window; the "
          L"picture is scaled to fit, with black bars if its shape differs. Takes effect after "
          L"a restart.",
          "RSX_WINDOW_MODE", { {0, L"Windowed"}, {1, L"Borderless fullscreen"} },
          [](const UiSettings& s) { return s.borderless ? 1 : 0; },
          [](UiSettings& s, int v) { s.borderless = v != 0; }, nullptr },
        { L"VIDEO", L"Antialiasing",
          L"Multisampling for what the game antialiases. The console used 2x; 4x is smoother "
          L"and costs a little speed.",
          "RSX_MSAA", { {1, L"Off"}, {2, L"2x"}, {4, L"4x"}, {8, L"8x"} },
          [](const UiSettings& s) { return s.msaa; },
          [](UiSettings& s, int v) { s.msaa = v; }, nullptr },
        { L"VIDEO", L"Vsync",
          L"Wait for the monitor's refresh when showing a frame. Off can tear, but never holds "
          L"the game back.",
          "RSX_PRESENT_SYNC", kOnOff,
          [](const UiSettings& s) { return s.vsync ? 1 : 0; },
          [](UiSettings& s, int v) { s.vsync = v != 0; },
          [](int v) { rsx_d3d12_set_present_sync(v ? 1 : 0); } },
        { L"SHADERS", L"Shader cache",
          L"Keep compiled shaders in the shadercache folder, so a screen seen before does not "
          L"stutter while they are rebuilt.",
          "RSX_SHADER_CACHE", kOnOff,
          [](const UiSettings& s) { return s.shader_cache ? 1 : 0; },
          [](UiSettings& s, int v) { s.shader_cache = v != 0; }, nullptr },
        { L"SHADERS", L"Background shader compilation",
          L"Build new shaders on background threads, all at once, instead of stopping the "
          L"picture to build them one by one. A new screen shows at once, its last details a "
          L"moment later. Needs the shader cache.",
          "RSX_SHADER_ASYNC", kOnOff,
          [](const UiSettings& s) { return s.shader_precompile ? 1 : 0; },
          [](UiSettings& s, int v) { s.shader_precompile = v != 0; }, nullptr },
        { L"SHADERS", L"Preload known shaders",
          L"In background (default): prepare learned menu, then race pipelines while the game "
          L"starts. Before start: prepare them all first, on every CPU core but one; startup "
          L"waits, with a progress badge. Needs shader cache; uses more memory; after a restart.",
          "RSX_SHADER_WARMUP", { {1, L"In background"}, {2, L"Before start"}, {0, L"Off"} },
          [](const UiSettings& s) { return !s.shader_warmup ? 0 : s.shader_warmup_startup ? 2 : 1; },
          [](UiSettings& s, int v) { s.shader_warmup = v != 0; s.shader_warmup_startup = v == 2; }, nullptr },
        { L"SHADERS", L"Background pipeline creation",
          L"Build driver pipelines on a background worker to reduce render stalls. "
          L"First-seen details can appear a moment later while preparation finishes.",
          "RSX_PSO_ASYNC", kOnOff,
          [](const UiSettings& s) { return s.pipeline_async ? 1 : 0; },
          [](UiSettings& s, int v) { s.pipeline_async = v != 0; }, nullptr },
        { L"SHADERS", L"Prepare race pipelines",
          L"Prepare the race pipelines learned on earlier visits during loading. "
          L"First visits can still compile new states. Needs background pipeline creation "
          L"and shader cache; may extend loading and use more memory.",
          "RSX_RACE_PREWARM", kOnOff,
          [](const UiSettings& s) { return s.race_preload ? 1 : 0; },
          [](UiSettings& s, int v) { s.race_preload = v != 0; }, nullptr },
        { L"SHADERS", L"Draw while compiling",
          L"While a shader compiles, draw with a general interpreter shader instead of leaving "
          L"the detail out or stopping the picture. Fewer missing details and hitches on first "
          L"visits; the stand-in costs some GPU time. Takes effect after a restart.",
          "RSX_SHADER_INTERP", kOnOff,
          [](const UiSettings& s) { return s.shader_interp ? 1 : 0; },
          [](UiSettings& s, int v) { s.shader_interp = v != 0; }, nullptr },
        { L"SHADERS", L"Shader compiler",
          L"DXC builds guest shaders for modern graphics cards. FXC restores the older compiler. "
          L"Each keeps its own cache; switching requires a restart and new shaders must be prepared.",
          "RSX_SHADER_COMPILER", { {0, L"FXC"}, {1, L"DXC"} },
          [](const UiSettings& s) { return s.shader_dxc ? 1 : 0; },
          [](UiSettings& s, int v) { s.shader_dxc = v != 0; }, nullptr },
        { L"INPUT", L"PlayStation / other controllers",
          L"Read DualSense, DualShock, Switch and other controllers through SDL2.dll. Xbox "
          L"controllers work either way.",
          "PAD_NO_SDL", kOnOff,
          [](const UiSettings& s) { return s.sdl_controllers ? 1 : 0; },
          [](UiSettings& s, int v) { s.sdl_controllers = v != 0; }, nullptr },
        { L"INPUT", L"Keyboard as controller",
          L"Play with the keyboard when no controller is connected. The keys are on the "
          L"Keyboard page (Z Cross, X Circle, Enter Start, arrows ... unless changed there).",
          "PAD_NO_KEYBOARD", kOnOff,
          [](const UiSettings& s) { return s.keyboard ? 1 : 0; },
          [](UiSettings& s, int v) { s.keyboard = v != 0; },
          [](int v) { pad_set_keyboard_enabled(v); } },
        { L"INPUT", L"Button prompts",
          L"The button symbols the game shows. Auto follows the controller in use: Xbox and "
          L"other pads, Switch pads and the keyboard get their own (Kenney's art); a "
          L"PlayStation pad keeps the game's.",
          "GT5P_GLYPHS", { {0, L"Auto"}, {1, L"PlayStation"}, {2, L"Xbox"}, {3, L"Nintendo"}, {4, L"Keyboard"} },
          [](const UiSettings& s) { return s.button_prompts; },
          [](UiSettings& s, int v) { s.button_prompts = v; },
          [](int v) { gt5p_glyphs_set_style(v); } },
        { L"CONTROLS", L"Modern controls",
          L"Changes the game's player-one assignments: R2/RT accelerate, L2/LT brake, Cross/A gear up and Square/X gear down. No restores these four defaults. Other assignments stay. Applies live; use the game's normal Save to keep it.",
          "PAD_MODERN_CONTROLS", { {0, L"No"}, {1, L"Yes"} },
          [](const UiSettings& s) { int v=gt5p_modern_controls_state(); return v<0?(int)s.modern_controls:v; },
          [](UiSettings& s, int v) { s.modern_controls = v != 0; },
          [](int v) { gt5p_modern_controls_request(v); } },
        { L"CONTROLS", L"Trigger dead zone",
          L"Removes the start of both triggers and rescales the remaining travel. Default 0% preserves existing input. Restart to apply.",
          "PAD_TRIGGER_DEADZONE", { {0, L"0%"}, {2, L"2%"}, {5, L"5%"}, {10, L"10%"}, {15, L"15%"}, {20, L"20%"} },
          [](const UiSettings& s) { return (int)s.trigger_deadzone; },
          [](UiSettings& s, int v) { s.trigger_deadzone = v; }, nullptr },
        { L"CONTROLS", L"Trigger response",
          L"Linear preserves existing response. Gentle gives finer control near rest; Quick reaches high pressure sooner. Both keep full travel. Restart to apply.",
          "PAD_TRIGGER_CURVE", { {0, L"Linear"}, {1, L"Gentle"}, {2, L"Quick"} },
          [](const UiSettings& s) { return (int)s.trigger_curve; },
          [](UiSettings& s, int v) { s.trigger_curve = v; }, nullptr },
        { L"FEEDBACK", L"DualSense fixed resistance",
          L"Experimental: fixed resistance on both DualSense triggers through SDL2, while focused and input is active. No vehicle telemetry. Bluetooth may need a controller power cycle before other apps can use DirectInput. Unsupported devices ignore it. Restart to apply.",
          "PAD_ADAPTIVE_TRIGGERS", kOnOff,
          [](const UiSettings& s) { return (int)s.adaptive_triggers; },
          [](UiSettings& s, int v) { s.adaptive_triggers = v; }, nullptr },
        { L"FEEDBACK", L"Xbox trigger rumble",
          L"Experimental: sends the game's large/small motor commands to supported left/right Xbox trigger motors through SDL2. No ABS or wheel-slip telemetry. Restart to apply.",
          "PAD_TRIGGER_RUMBLE", kOnOff,
          [](const UiSettings& s) { return (int)s.trigger_rumble; },
          [](UiSettings& s, int v) { s.trigger_rumble = v; }, nullptr },
        { L"FEEDBACK", L"SDL controller vibration",
          L"Forwards the game's vibration commands to controller body motors. Also enable vibration in the game's own controller options. Bluetooth DualSense uses enhanced reports; power cycle it before using DirectInput in another app if needed. Restart to apply.",
          "PAD_SDL_RUMBLE", kOnOff,
          [](const UiSettings& s) { return (int)s.sdl_rumble; },
          [](UiSettings& s, int v) { s.sdl_rumble = v; }, nullptr },
        KEY_ITEM(0, L"Cross"),  KEY_ITEM(1, L"Circle"), KEY_ITEM(2, L"Square"), KEY_ITEM(3, L"Triangle"),
        KEY_ITEM(4, L"L1"),     KEY_ITEM(5, L"R1"),     KEY_ITEM(6, L"L2"),     KEY_ITEM(7, L"R2"),
        KEY_ITEM(8, L"L3"), KEY_ITEM(9, L"R3"), KEY_ITEM(10, L"Start"), KEY_ITEM(11, L"Select"),
        KEY_ITEM(12, L"Up"),    KEY_ITEM(13, L"Down"),  KEY_ITEM(14, L"Left"),  KEY_ITEM(15, L"Right"),
        { L"WHEEL", L"Steering wheel",
          L"A steering wheel (Logitech, Thrustmaster, Fanatec and others) is shown to the game as a "
          L"Logitech G27, with force feedback. Plug it in before starting; a controller or the keyboard "
          L"still works alongside. Restart to apply.",
          "PS3_WHEEL", kOnOff,
          [](const UiSettings& s) { return s.wheel ? 1 : 0; },
          [](UiSettings& s, int v) { s.wheel = v != 0; }, nullptr },
        { L"WHEEL", L"Wheel preset",
          L"Quick axis and button layout for common wheels. Auto detects Logitech G29/G920/G27 and "
          L"Thrustmaster by USB ID; fine-tune individual axes and buttons on the Wheel map tab. Applies live.",
          nullptr, { {kPresetAuto, L"Auto (by model)"}, {kPresetG29, L"Logitech G29 / G923 PS"},
                     {kPresetG920, L"Logitech G920 / G923 Xbox"}, {kPresetG27, L"Logitech G25 / G27 / DFGT"},
                     {kPresetThrustmaster, L"Thrustmaster (separate)"}, {kPresetCustom, L"Custom"} },
          [](const UiSettings& s) { return wheel_preset_id(s.wheel_preset); },
          [](UiSettings& s, int v) { wheel_apply_preset_values(s, v); },
          [](int) { /* pushed in set_value via wheel_host_set_map */ } },
        { L"WHEEL", L"Wheel rotation",
          L"Degrees your wheel turns lock to lock, as set in its own software (Logitech G HUB: 900). "
          L"The game sets each car's own rotation from this. Restart to apply.",
          "PS3_WHEEL_RANGE", { {270, L"270"}, {360, L"360"}, {540, L"540"}, {720, L"720"}, {900, L"900"}, {1080, L"1080"} },
          [](const UiSettings& s) { return s.wheel_range; },
          [](UiSettings& s, int v) { s.wheel_range = v; }, nullptr },
        { L"WHEEL", L"Force feedback strength",
          L"How hard the game's forces push, from Off to 100%. The game's own Options also has a "
          L"force feedback setting; both apply. Applies live.",
          "PS3_WHEEL_FFB", { {0, L"Off"}, {10, L"10%"}, {20, L"20%"}, {30, L"30%"}, {40, L"40%"}, {50, L"50%"},
                             {60, L"60%"}, {70, L"70%"}, {80, L"80%"}, {90, L"90%"}, {100, L"100%"} },
          [](const UiSettings& s) { return s.ffb_strength; },
          [](UiSettings& s, int v) { s.ffb_strength = v; },
          [](int v) { wheel_ffb_apply(kFfbGain, v); } },
        { L"WHEEL", L"Force direction",
          L"Inverted if the wheel pulls away from the centre in a corner instead of towards it. Applies live.",
          "PS3_WHEEL_FFB_INVERT", { {0, L"Normal"}, {1, L"Inverted"} },
          [](const UiSettings& s) { return s.ffb_invert ? 1 : 0; },
          [](UiSettings& s, int v) { s.ffb_invert = v != 0; },
          [](int v) { wheel_ffb_apply(kFfbInvert, v); } },
        { L"WHEEL", L"Centring and damping",
          L"The game's centring spring, damping and friction, as a share of what it asks for. Lower "
          L"makes the wheel lighter at low speed and in the menus. Applies live.",
          "PS3_WHEEL_FFB_SPRING", { {0, L"Off"}, {25, L"25%"}, {50, L"50%"}, {75, L"75%"}, {100, L"100%"},
                                    {125, L"125%"}, {150, L"150%"}, {200, L"200%"} },
          [](const UiSettings& s) { return s.ffb_springs; },
          [](UiSettings& s, int v) { s.ffb_springs = v; },
          [](int v) { wheel_ffb_apply(kFfbSpring, v); } },
        { L"WHEEL", L"Minimum force",
          L"Lifts small steering forces to at least this much, so gear and belt wheels (G29, G920, T300) "
          L"don't hide them in their own friction. Off for direct-drive wheels. Applies live.",
          "PS3_WHEEL_FFB_MIN", { {0, L"Off"}, {5, L"5%"}, {10, L"10%"}, {15, L"15%"}, {20, L"20%"}, {30, L"30%"} },
          [](const UiSettings& s) { return s.ffb_min_force; },
          [](UiSettings& s, int v) { s.ffb_min_force = v; },
          [](int v) { wheel_ffb_apply(kFfbMin, v); } },
        { L"WHEEL", L"Pause forces in background",
          L"No forces while this page is open or the game window is not in front, so the wheel never "
          L"pulls while you are elsewhere. Restart to apply.",
          "PS3_WHEEL_FFB_MUTE", kOnOff,
          [](const UiSettings& s) { return s.ffb_pause ? 1 : 0; },
          [](UiSettings& s, int v) { s.ffb_pause = v != 0; }, nullptr },
        WMAP_AXIS(0, L"Steer axis"), WMAP_AXIS(1, L"Throttle"), WMAP_AXIS(2, L"Brake"), WMAP_AXIS(3, L"Clutch"),
        WMAP_BTN(4, L"Shift Up (R1)"), WMAP_BTN(5, L"Shift Down (L1)"), WMAP_BTN(6, L"Cross"), WMAP_BTN(7, L"Circle"),
        WMAP_BTN(8, L"Square"), WMAP_BTN(9, L"Triangle"), WMAP_BTN(10, L"L2"), WMAP_BTN(11, L"R2"),
        WMAP_BTN(12, L"L3"), WMAP_BTN(13, L"R3"), WMAP_BTN(14, L"Start"), WMAP_BTN(15, L"Select"),
        WMAP_BTN(16, L"D-Pad Up"), WMAP_BTN(17, L"D-Pad Down"), WMAP_BTN(18, L"D-Pad Left"), WMAP_BTN(19, L"D-Pad Right"),
        { L"GAME", L"Game version",
          L"Restart into an installed version. Each keeps separate game data, saves and shaders. 2.11 is experimental. Set it up through the launcher before switching.",
          "GT5P_GAME_VERSION", {{104,L"GT5 01.04"},{211,L"GT5 2.11 (experimental)"}},
          [](const UiSettings& s) { return s.launcher_version; },
          [](UiSettings& s,int v) { s.launcher_version=v; }, nullptr },
        { L"GAME", L"Mod",
          L"A data mod loaded over this version's game data, such as GT5 Master Mod on 2.11. "
          L"Off is the unmodded game; your installed data is never changed. Mods installs or "
          L"removes them. Turning one on keeps a copy of your save first. Restart to apply.",
          "GT5P_MOD", mod_choices(),
          [](const UiSettings& s) { return s.launcher_mod; },
          [](UiSettings& s, int v) { s.launcher_mod = v; }, nullptr },

        { L"GAME", L"Skip opening movie",
          L"Press through the opening movie as soon as it starts. It only plays when the TV "
          L"content is installed.",
          "PAD_SKIP_MOVIE", kOnOff,
          [](const UiSettings& s) { return s.skip_movie ? 1 : 0; },
          [](UiSettings& s, int v) { s.skip_movie = v != 0; }, nullptr },
        { L"GAME", L"Smooth colour",
          L"Render at 16 bits per channel and drop the game's dither noise: dark areas are "
          L"clean, without banding. Off is the game's own 8-bit picture with its noise. Takes "
          L"effect after a restart.",
          "RSX_HIGH_PRECISION", kOnOff,
          [](const UiSettings& s) { return s.smooth_colour ? 1 : 0; },
          [](UiSettings& s, int v) { s.smooth_colour = v != 0; }, nullptr },
        { L"GAME", L"Texture filtering",
          L"Anisotropic filtering keeps the road, kerbs and boards sharp into the distance; the "
          L"game's own is trilinear, which blurs surfaces seen at a slant. Takes effect after a "
          L"restart.",
          "RSX_ANISO", { {1, L"Trilinear"}, {4, L"4x anisotropic"}, {8, L"8x anisotropic"}, {16, L"16x anisotropic"} },
          [](const UiSettings& s) { return s.anisotropy; },
          [](UiSettings& s, int v) { s.anisotropy = v; }, nullptr },
        { L"GAME", L"Looser chase camera",
          L"RPCS3 Looser Chase Camera patch: lets the chase camera sway freely with steering "
          L"and drift instead of locking rigidly behind the car. Applies live.",
          "GT5_PATCH_LOOSER_CHASE_CAMERA", kOnOff,
          [](const UiSettings& s) { return s.looser_chase_camera ? 1 : 0; },
          [](UiSettings& s, int v) { s.looser_chase_camera = v != 0; },
          [](int) { /* pushed in set_value via gt5p_patches_apply_chase_cam */ } },
        { L"GAME", L"Chase cam damper",
          L"Spring damper for Looser chase camera. Lower values let the camera swing further "
          L"(90 default, 300 vanilla). Applies live when Looser chase camera is On.",
          "GT5_CHASE_CAM_DAMPER", { {30, L"30 (Very loose)"}, {60, L"60 (Loose)"}, {90, L"90 (Default)"},
                                    {120, L"120"}, {150, L"150 (Medium)"}, {200, L"200 (Firm)"}, {300, L"300 (Vanilla)"} },
          [](const UiSettings& s) { return s.chase_cam_damper; },
          [](UiSettings& s, int v) { s.chase_cam_damper = v; },
          [](int) { /* pushed in set_value via gt5p_patches_apply_chase_cam */ } },
        { L"GAME", L"Chase cam max angle",
          L"Maximum rotation angle for Looser chase camera (35 default, 30 vanilla). "
          L"Applies live when Looser chase camera is On.",
          "GT5_CHASE_CAM_MAX_ANGLE", { {15, L"15"}, {25, L"25"}, {30, L"30 (Vanilla)"},
                                       {35, L"35 (Default)"}, {45, L"45"}, {60, L"60"}, {80, L"80"} },
          [](const UiSettings& s) { return s.chase_cam_max_angle; },
          [](UiSettings& s, int v) { s.chase_cam_max_angle = v; },
          [](int) { /* pushed in set_value via gt5p_patches_apply_chase_cam */ } },
        { L"GAME", L"Unlock FPS",
          L"RPCS3 Unlock FPS patch (GT5 2.11): removes the 30/60 Hz flip-interval cap in "
          L"menus and races. Off by default. Applies live.",
          "GT5_PATCH_UNLOCK_FPS", kOnOff,
          [](const UiSettings& s) { return s.unlock_fps ? 1 : 0; },
          [](UiSettings& s, int v) { s.unlock_fps = v != 0; },
          [](int v) { gt5p_patches_apply_unlock_fps(v != 0); } },
        { L"AUDIO", L"Sound output",
          L"GT5 mixes 5.1 surround. Stereo folds it down. 5.1 or 7.1 sends it to surround "
          L"speakers (7.1's side pair stays silent); Auto follows the Windows speaker setup. Restart to apply.",
          "AUDIO_OUTPUT_CHANNELS", { {2, L"Stereo"}, {6, L"5.1 surround"}, {8, L"7.1 surround"}, {0, L"Auto"} },
          [](const UiSettings& s) { return s.audio_channels; },
          [](UiSettings& s, int v) { s.audio_channels = v; }, nullptr },
        { L"GAME", L"Shader compile indicator",
          L"Show \"Compiling shaders\" in the corner while a draw is waiting on a shader or "
          L"pipeline. Background preloading is not counted here.",
          "GT5P_SHADER_INDICATOR", kOnOff,
          [](const UiSettings& s) { return s.shader_indicator ? 1 : 0; },
          [](UiSettings& s, int v) { s.shader_indicator = v != 0; },
          [](int v) { gt5p_shader_indicator_enable(v); } },
        { L"GAME", L"Shader preload indicator",
          L"Show a smaller, quieter \"Loading shaders\" in the corner while the game prepares "
          L"known shaders in the background. Off by default; \"Compiling shaders\" takes its place "
          L"when both would show.",
          "GT5P_PRELOAD_INDICATOR", kOnOff,
          [](const UiSettings& s) { return s.preload_indicator ? 1 : 0; },
          [](UiSettings& s, int v) { s.preload_indicator = v != 0; },
          [](int v) { gt5p_preload_indicator_enable(v); } },
        { L"PERFORMANCE", L"Frame clock",
          L"The vblank the game paces its frames to. 59.94 Hz is the console's (30 fps at "
          L"most). 62.5 Hz allows 31 fps, but a frame that just misses two vblanks drops to 20.",
          "GT5P_VBLANK_HZ", { {5994, L"59.94 Hz"}, {6250, L"62.5 Hz"} },
          [](const UiSettings& s) { return s.vblank_hz100; },
          [](UiSettings& s, int v) { s.vblank_hz100 = v; }, nullptr },
        { L"PERFORMANCE", L"SPU lanes on own cores",
          L"Keep each SPU render lane on a CPU core of its own. Strict (the default) is about "
          L"14% faster; Hint gains nothing; Off leaves them to Windows.",
          "SPURS_LANE_PIN", { {0, L"Off"}, {1, L"Hint"}, {2, L"Strict"} },
          [](const UiSettings& s) { return s.lane_cores; },
          [](UiSettings& s, int v) { s.lane_cores = v; }, nullptr },
        /* "Fifth SPU lane" (fifth_spu_lane, SPURS_JL_SPU3) is left off the page:
         * it cannot work, as SPU 3's own bookkeeping row is the render lists'
         * progress row (Phase 64). The file key stays, for testing. */
        { L"DEBUG", L"Runtime log",
          L"The full runtime log. Auto keeps it off for all launch methods. On makes "
          L"the game stutter.",
          "PS3_VERBOSE", { {-1, L"Auto"}, {0, L"Off"}, {1, L"On"} },
          [](const UiSettings& s) { return s.verbose_log; },
          [](UiSettings& s, int v) { s.verbose_log = v; }, nullptr },
        { L"DEBUG", L"Debug export",
          L"Keep recent logs and enable Export debug report in F3 (or F8 while F3 is open). "
          L"Saves a ZIP to logs/debug to attach to bug reports. Capture starts when enabled.",
          nullptr, kOnOff,
          [](const UiSettings& s) { return s.debug_export ? 1 : 0; },
          [](UiSettings& s, int v) { s.debug_export = v != 0; },
          [](int v) { gt5p_debug_enable(v); } },
    };
    return v;
}

/* ---- layout ----
 *
 * The page is one of the game's settings popups (its "Steering Settings"
 * dialog), and the quit question its "Confirmation" popup: a flat grey-28
 * panel over the dimmed scene, a centred title, rows of a right-aligned label,
 * a black value box with a grey underline and a square drop-down button (red
 * ring on the selected row), dotted rules between rows, and pill buttons
 * between two grey-76 rules. Sections are pages, switched with L1/R1 (Q/W,
 * Page Up/Down) or on the bar under the title. Units are 1280 x 720 (the
 * backbuffer's pixels over 1.5 at 1080p); sizes and greys were measured from
 * the game's screens (Phases 115-116). */
const int kW = 1280, kH = 720;
const int kPx0 = 250, kPx1 = 1030, kPy0 = 84, kPy1 = 640;     /* the panel */
const int kBarY = 124, kBarH = 30, kRule1 = 168;              /* section bar, rule under it */
const int kRowY = 186, kRowH = 54;                            /* first row, pitch */
const int kFieldX = 650, kFieldW = 250, kFieldH = 32, kDropW = 33;
const int kHelpY = 468, kRule2 = 536;
const int kBtnW = 157, kBtnH = 37, kBtnY = 556, kBtnGap = 16;

HWND s_game, s_wnd;
bool s_open;
bool s_overlay;                 /* drawn into the game's picture (this open) */
double s_scale = 1;             /* overlay: backbuffer pixels per layout unit */
double s_scale_x = 1;           /* the same across (s_scale x paint_ax()) */
int s_ox, s_oy;                 /* overlay: its top left in the backbuffer */
int s_sel;                      /* row: 0 the section bar, then items, then buttons */
int s_section;                  /* the page shown */
bool s_drop;                    /* the selected item's list is open */
int s_drop_sel;                 /* the highlighted choice in it */
UiSettings s_edit;   /* what the page shows and saves */
UiSettings s_boot;   /* the file as this run started with it */
std::wstring s_status;
bool s_quit;                    /* the quit question is up */
int s_quit_sel;                 /* 0 Cancel, 1 OK */
volatile LONG s_quit_req;       /* Escape pressed in the game window */
int s_capture = -1;             /* a key item waiting for its new key */
uint8_t s_cap_was[256];         /* keys held when the wait began */
int s_wcap = -1;                /* a wheel-map item waiting for an axis or button */
WheelRawUi s_wcap_base = {};    /* wheel axes/buttons when wheel capture began */

enum { kRowRestart = -1, kRowClose = -2, kRowHead = -3, kRowDefaults = -4, kRowSetup211 = -5, kRowMods = -6 };
bool is_button(int id) { return id == kRowRestart || id == kRowClose || id == kRowDefaults || id == kRowSetup211 || id == kRowMods; }

/* The Keyboard & Wheel Map pages: sixteen bindings in two columns of eight. */
const int kKeyY = 180, kKeyH = 34, kKeyFieldW = 130;
bool keyboard_page();
bool wheel_page();
bool wheel_map_page();

bool legacy_ui_flag()
{
    static int legacy = -1;
    if (legacy < 0) { const char* e = getenv("GT5P_SETTINGS_UI_LEGACY"); legacy = e && e[0] == '1'; }
    return legacy != 0;
}

bool restart_pending()
{
    for (const Item& it : items())
        if (!it.apply && (launcher_item(it) || !gt5p_settings_overridden(it.env)) && it.get(s_edit) != it.get(s_boot))
            return true;
    return false;
}

/* The sections, in the order they first appear in items(). */
const std::vector<const wchar_t*>& sections()
{
    static std::vector<const wchar_t*> v;
    if (v.empty())
        for (const Item& it : items()) {
            bool seen = false;
            for (const wchar_t* s : v) seen |= !wcscmp(s, it.section);
            if (!seen) v.push_back(it.section);
        }
    return v;
}

/* Rows in order: the section bar, this page's items, then the buttons
 * ("Restart" when needed, "Close"). */
std::vector<int> rows()
{
    std::vector<int> r = { kRowHead };
    const wchar_t* sec = sections()[s_section];
    for (int i = 0; i < (int)items().size(); i++) if (!wcscmp(items()[i].section, sec) && (!launcher_item(items()[i]) || pt5_restart::managed()) &&
            (!env_is(items()[i], "GT5P_MOD") || mods_available()) &&
            (gt5p_patches_has_chase_cam() || !(env_is(items()[i], "GT5_PATCH_LOOSER_CHASE_CAMERA") ||
                env_is(items()[i], "GT5_CHASE_CAM_DAMPER") || env_is(items()[i], "GT5_CHASE_CAM_MAX_ANGLE"))) &&
            (gt5p_patches_has_unlock_fps() || !env_is(items()[i], "GT5_PATCH_UNLOCK_FPS"))) r.push_back(i);
    const char* ready211=getenv("GT5P_VERSION_211_READY");
    const char* supported211=getenv("GT5P_VERSION_211_SUPPORTED");
    if (pt5_restart::managed() && !wcscmp(sec,L"GAME") && supported211 && !strcmp(supported211,"1") && (!ready211 || strcmp(ready211,"1"))) r.push_back(kRowSetup211);
    if (!wcscmp(sec, L"GAME") && mods_available()) r.push_back(kRowMods);
    if (restart_pending()) r.push_back(kRowRestart);
    if (keyboard_page() || wheel_map_page()) r.push_back(kRowDefaults);
    r.push_back(kRowClose);
    return r;
}

bool keyboard_page() { return !wcscmp(sections()[s_section], L"KEYBOARD"); }
bool wheel_page() { return !wcscmp(sections()[s_section], L"WHEEL"); }
bool wheel_map_page() { return !wcscmp(sections()[s_section], L"WHEEL MAP"); }

/* Where each row sits: items down the panel, buttons side by side, centred. */
struct Slot { int x0, x1, y; };
std::vector<Slot> layout(const std::vector<int>& r)
{
    std::vector<Slot> out;
    int nbtn = 0, nitems = 0, y = kRowY;
    for (int id : r) {
        nbtn += is_button(id);
        nitems += id >= 0 && items()[id].key < 0 && items()[id].wmap < 0;
    }
    /* Six shader rows must leave space for the selected row's help. Keyboard
     * and wheel bindings retain their separate two-column layout. */
    const int available_pitch = nitems > 1 ? (kHelpY - 12 - kFieldH - kRowY) / (nitems - 1) : kRowH;
    const int pitch = available_pitch < kRowH ? available_pitch : kRowH;
    int bx = kW / 2 - (nbtn * kBtnW + (nbtn - 1) * kBtnGap) / 2;
    for (int id : r) {
        if (id == kRowHead) out.push_back({ kPx0 + 40, kPx1 - 40, kBarY });
        else if (id >= 0 && items()[id].key >= 0) {
            const int k = items()[id].key, col = k / 8;
            const int x0 = col ? kW / 2 + 10 : kPx0 + 30;
            out.push_back({ x0, x0 + (kPx1 - kPx0) / 2 - 40, kKeyY + (k % 8) * kKeyH });
        }
        else if (id >= 0 && items()[id].wmap >= 0) {
            const int k = items()[id].wmap, col = k / 10;
            const int x0 = col ? kW / 2 + 10 : kPx0 + 30;
            out.push_back({ x0, x0 + (kPx1 - kPx0) / 2 - 40, 180 + (k % 10) * 28 });
        }
        else if (id >= 0) { out.push_back({ kPx0 + 40, kPx1 - 40, y }); y += pitch; }
        else { out.push_back({ bx, bx + kBtnW, kBtnY }); bx += kBtnW + kBtnGap; }
    }
    return out;
}

int choice_index(const Item& it)
{
    const int v = it.get(s_edit);
    for (int i = 0; i < (int)it.choices.size(); i++) if (it.choices[i].value == v) return i;
    return 0;
}

std::wstring widen(const std::string& s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

void relaunch()
{
    if (pt5_restart::managed()) {
        fflush(stdout); fflush(stderr);
        /* Launcher waits, then restarts under its existing storage lease. A
         * version change wins over a mod change (mods belong to a version). */
        ExitProcess(s_edit.launcher_version != s_boot.launcher_version ? (s_edit.launcher_version == 211 ? pt5_restart::version_211 : pt5_restart::version_104)
                    : s_edit.launcher_mod != s_boot.launcher_mod ? pt5_restart::mod_select | (unsigned long)(s_edit.launcher_mod & 0xFF)
                    : pt5_restart::exit_code);
    }
    /* The environment holds the variables loading the old file set; the new
     * process must read the new file instead. Clear the ones it set. */
    for (const Item& it : items())
        if (it.env && !gt5p_settings_overridden(it.env)) _putenv_s(it.env, "");
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    std::wstring cmd = GetCommandLineW();
    STARTUPINFOW si = {}; si.cb = sizeof si;
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(exe, &cmd[0], NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        fflush(stdout); fflush(stderr);
        TerminateProcess(GetCurrentProcess(), 0);
    }
    s_status = L"Could not start the game again; restart it by hand.";
}

void set_open(bool open);
void set_value(int id, int v);

void reset_keys();
void reset_wheel_map();
void wheel_map_push();

void change(int id, int dir)
{
    if (id == kRowClose) { set_open(false); return; }
    if (id == kRowRestart) { relaunch(); return; }
    if (id == kRowSetup211) {
        if (MessageBoxW(nullptr,L"Close the game and open GT5 2.11 setup? Your 01.04 installation and saves will be kept. Finish or save your current session before continuing.",L"Set up GT5 2.11",MB_OKCANCEL|MB_ICONQUESTION)==IDOK) { fflush(stdout); fflush(stderr); ExitProcess(pt5_restart::setup_211); }
        return;
    }
    if (id == kRowMods) {
        if (MessageBoxW(nullptr,L"Close the game and open the mod manager? It installs a mod from a ZIP file or removes one, then starts the game again. Finish or save your current session before continuing.",L"Game mods",MB_OKCANCEL|MB_ICONQUESTION)==IDOK) { fflush(stdout); fflush(stderr); ExitProcess(pt5_restart::mods_manage); }
        return;
    }
    if (id == kRowDefaults) { if (wheel_map_page()) reset_wheel_map(); else reset_keys(); return; }
    const Item& it = items()[id];
    if (it.key >= 0 || it.wmap >= 0) return;
    const int n = (int)it.choices.size();
    set_value(id, it.choices[(choice_index(it) + dir + n) % n].value);
}

/* Store an item's new value, apply it when it can be live, and say so. */
void set_value(int id, int v)
{
    const Item& it = items()[id];
    if (env_is(it,"GT5P_GAME_VERSION")) {
        const char* ready=getenv(v == 211 ? "GT5P_VERSION_211_READY" : "GT5P_VERSION_104_READY");
        if (!pt5_restart::managed() || !ready || strcmp(ready,"1")) { s_status=L"Set up this game version in the launcher first.";return; }
        it.set(s_edit,v); s_status=L"Select Restart to change game version. Saves stay separate.";return;
    }
    if (env_is(it,"GT5P_MOD")) {
        if (!mods_available()) { s_status=L"Start the game from tourline5.exe to use mods.";return; }
        it.set(s_edit,v);
        s_status = v == s_boot.launcher_mod ? L"No change." : !v ? L"Select Restart to play without mods."
                 : s_boot.launcher_mod ? L"Select Restart to load this mod." : L"Select Restart to load this mod. A copy of your save is kept first.";
        return;
    }
    if (it.env && !strcmp(it.env,"PAD_MODERN_CONTROLS") && gt5p_modern_controls_state()<0) {
        s_status=gt5p_modern_controls_state()==-2 ? L"Native controls link is unavailable for this build." : L"Wait for the game to load its controller profile.";
        return;
    }
    it.set(s_edit, v);
    const bool over = !launcher_item(it) && gt5p_settings_overridden(it.env);
    if (!gt5p_settings_enabled()) s_status = L"GT5P_NO_SETTINGS is set: changes are not saved.";
    else if (!gt5p_settings_store(s_edit)) s_status = L"Could not write " + widen(gt5p_settings_path());
    else s_status = L"Saved.";
    if (over) s_status += L"  " + widen(it.env) + L" is set in the environment and wins over this.";
    else if (it.apply) {
        if (!wcscmp(it.label, L"Wheel preset")) wheel_map_push();
        else if (it.env && (!strcmp(it.env, "GT5_PATCH_LOOSER_CHASE_CAMERA") ||
                            !strcmp(it.env, "GT5_CHASE_CAM_DAMPER") ||
                            !strcmp(it.env, "GT5_CHASE_CAM_MAX_ANGLE")))
            gt5p_patches_apply_chase_cam(s_edit.looser_chase_camera, s_edit.chase_cam_damper, s_edit.chase_cam_max_angle);
        else it.apply(v);
        s_status += it.env && !strcmp(it.env,"PAD_MODERN_CONTROLS") ?
            L"  Mapping requested. Use the game's normal Save to keep it." : L"  Applied.";
    }
    else if (it.get(s_edit) != it.get(s_boot)) s_status += L"  Takes effect after a restart.";
}

/* The item for button `k`'s binding. */
int key_item(int k)
{
    for (int i = 0; i < (int)items().size(); i++) if (items()[i].key == k) return i;
    return -1;
}

/* Bind button k to vk; a button that had vk gets k's old key. */
void assign_key(int k, int vk)
{
    const int old = s_edit.keys[k];
    std::wstring moved;
    for (int j = 0; j < GT5P_KEY_BUTTONS; j++)
        if (j != k && vk && gt5p_key_normalize(s_edit.keys[j]) == gt5p_key_normalize(vk)) {
            set_value(key_item(j), old);
            moved = widen(gt5p_key_name(vk)) + L" moved from " + gt5p_key_button(j).label + L"; " +
                    gt5p_key_button(j).label + L" is now " + widen(gt5p_key_name(old) ? gt5p_key_name(old) : "None") + L".  ";
        }
    set_value(key_item(k), vk);
    if (!moved.empty()) s_status = moved + s_status;
}

void reset_keys()
{
    for (int k = 0; k < GT5P_KEY_BUTTONS; k++) set_value(key_item(k), gt5p_key_button(k).default_vk);
    s_status = L"Default keys.  " + s_status;
}

/* The key-wait's result: a key, Escape to cancel, or a key the port keeps. */
void capture_key(int vk)
{
    if (s_capture < 0) return;
    if (vk == VK_ESCAPE) { s_capture = -1; s_status = L"Not changed."; return; }
    vk = gt5p_key_normalize(vk);
    if (!gt5p_key_name(vk)) { s_status = L"That key cannot be bound (Escape and F1-F12 belong to the port)."; return; }
    const int k = items()[s_capture].key;
    s_capture = -1;
    assign_key(k, vk);
}

void begin_capture(int id)
{
    s_capture = id;
    for (int vk = 0; vk < 256; vk++) s_cap_was[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
    s_status = L"";
}

/* ---- interactive wheel mapping (WHEEL MAP tab) ---- */

int wmap_item(int slot)
{
    for (int i = 0; i < (int)items().size(); i++) if (items()[i].wmap == slot) return i;
    return -1;
}

void wheel_map_push()
{
    wheel_host_set_map(s_edit.wheel_steer.c_str(), s_edit.wheel_pedals.c_str(), s_edit.wheel_buttons.c_str());
}

void wheel_map_save_and_apply(const std::wstring& msg)
{
    if (!gt5p_settings_enabled()) s_status = msg + L"  (GT5P_NO_SETTINGS: not saved)";
    else if (!gt5p_settings_store(s_edit)) s_status = L"Could not write " + widen(gt5p_settings_path());
    else s_status = msg + L"  Saved and applied.";
    wheel_map_push();
}

void reset_wheel_map()
{
    wheel_apply_preset_values(s_edit, kPresetAuto);
    wheel_map_save_and_apply(L"Wheel map reset to Auto.");
}

std::wstring wheel_slot_display(const UiSettings& s, int slot)
{
    if (slot == 0) {
        if (s.wheel_steer.empty()) return L"Auto (X)";
        return widen(s.wheel_steer);
    }
    if (slot >= 1 && slot <= 3) {
        std::string v = wheel_kv_get(s.wheel_pedals, kWheelPedalKeys[slot - 1]);
        if (v.empty()) return L"Auto";
        std::string up = v;
        for (char& c : up) c = (char)toupper((unsigned char)c);
        if (up == "NONE") return L"None";
        size_t inv = up.find("INV");
        if (inv != std::string::npos) return widen(up.substr(0, inv)) + L" (inv)";
        return widen(up);
    }
    if (slot >= 4 && slot < 20) {
        std::string v = wheel_kv_get(s.wheel_buttons, kWheelBtnSlots[slot - 4].key);
        if (v.empty()) return L"Auto";
        int b = atoi(v.c_str());
        if (b <= 0) return L"None";
        if (b > 128) {
            const int aux_i = (b - 129) / 32 + 1;
            const int aux_b = (b - 129) % 32 + 1;
            return L"Aux" + std::to_wstring(aux_i) + L" Btn " + std::to_wstring(aux_b);
        }
        return L"Btn " + std::to_wstring(b);
    }
    return L"?";
}

void begin_wheel_capture(int id)
{
    WheelRawUi raw = {};
    if (!wheel_host_raw(&raw) || !raw.connected) {
        s_status = L"No wheel detected — plug in your wheel, or pick a preset on the Wheel tab.";
        return;
    }
    s_wcap = id;
    s_wcap_base = raw;
    s_status = L"";
}

void wheel_clear_slot(int id)
{
    const int slot = items()[id].wmap;
    if (slot < 0) return;
    s_edit.wheel_preset = "custom";
    if (slot == 0) {
        s_edit.wheel_steer.clear();
        wheel_map_save_and_apply(L"Steer axis set to Auto.");
    } else if (slot >= 1 && slot <= 3) {
        const char* pk = kWheelPedalKeys[slot - 1];
        std::string cur = wheel_kv_get(s_edit.wheel_pedals, pk);
        /* Toggle between None (for clutch/unused) and Auto on repeated Delete. */
        std::string next = (_stricmp(cur.c_str(), "NONE") == 0) ? "" : "NONE";
        s_edit.wheel_pedals = wheel_kv_set(s_edit.wheel_pedals, pk, next);
        wheel_map_save_and_apply(std::wstring(items()[id].label) + (next.empty() ? L" set to Auto." : L" cleared."));
    } else if (slot >= 4 && slot < 20) {
        s_edit.wheel_buttons = wheel_kv_set(s_edit.wheel_buttons, kWheelBtnSlots[slot - 4].key, "0");
        wheel_map_save_and_apply(std::wstring(items()[id].label) + L" cleared.");
    }
}

void wheel_invert_pedal_slot(int id)
{
    const int slot = items()[id].wmap;
    if (slot < 1 || slot > 3) return;
    const char* pk = kWheelPedalKeys[slot - 1];
    std::string cur = wheel_kv_get(s_edit.wheel_pedals, pk);
    if (cur.empty() || _stricmp(cur.c_str(), "NONE") == 0) {
        /* Start from default G29 axis if unset, then invert. */
        static const char* const def_ax[3] = { "Y", "RZ", "S0" };
        cur = def_ax[slot - 1];
    }
    std::string up = cur;
    for (char& c : up) c = (char)toupper((unsigned char)c);
    size_t inv = up.find("INV");
    std::string next = (inv != std::string::npos) ? up.substr(0, inv) : (up + "inv");
    s_edit.wheel_preset = "custom";
    s_edit.wheel_pedals = wheel_kv_set(s_edit.wheel_pedals, pk, next);
    wheel_map_save_and_apply(std::wstring(items()[id].label) + L" -> " + wheel_slot_display(s_edit, slot) + L".");
}

/* Called each UI tick while waiting for a wheel axis or button. Returns true when captured. */
bool poll_wheel_capture()
{
    if (s_wcap < 0) return false;
    WheelRawUi now = {};
    if (!wheel_host_raw(&now) || !now.connected) {
        s_wcap = -1;
        s_status = L"Wheel disconnected.";
        return true;
    }
    const int slot = items()[s_wcap].wmap;
    if (slot >= 0 && slot <= 3) {
        int best = -1;
        float best_d = 0.22f;
        for (int a = 0; a < 8; a++) {
            const float d = fabsf(now.axis[a] - s_wcap_base.axis[a]);
            if (d > best_d) { best_d = d; best = a; }
        }
        if (best >= 0) {
            const int id = s_wcap;
            s_wcap = -1;
            s_edit.wheel_preset = "custom";
            if (slot == 0) {
                s_edit.wheel_steer = kWheelAxisNames[best];
            } else {
                s_edit.wheel_pedals = wheel_kv_set(s_edit.wheel_pedals, kWheelPedalKeys[slot - 1], kWheelAxisNames[best]);
            }
            wheel_map_save_and_apply(std::wstring(items()[id].label) + L" -> " + widen(kWheelAxisNames[best]) + L".");
            return true;
        }
    } else if (slot >= 4 && slot < 20) {
        int btn1 = 0;
        const uint32_t newly0 = now.buttons & ~s_wcap_base.buttons;
        if (newly0) {
            for (int b = 0; b < 32; b++) if (newly0 & (1u << b)) { btn1 = b + 1; break; }
        }
        for (int w = 0; !btn1 && w < 7; w++) {
            const uint32_t newly = now.buttons_ext[w] & ~s_wcap_base.buttons_ext[w];
            if (!newly) continue;
            for (int b = 0; b < 32; b++) if (newly & (1u << b)) { btn1 = 32 * (w + 1) + b + 1; break; }
        }
        if (btn1 > 0) {
            const int id = s_wcap;
            s_wcap = -1;
            s_edit.wheel_preset = "custom";
            const std::string num = std::to_string(btn1);
            /* Clear any other slot in WHEEL MAP that already used this button. */
            for (int j = 0; j < 16; j++) {
                if (j + 4 == slot) continue;
                std::string ex = wheel_kv_get(s_edit.wheel_buttons, kWheelBtnSlots[j].key);
                if (!ex.empty() && atoi(ex.c_str()) == btn1)
                    s_edit.wheel_buttons = wheel_kv_set(s_edit.wheel_buttons, kWheelBtnSlots[j].key, "0");
            }
            s_edit.wheel_buttons = wheel_kv_set(s_edit.wheel_buttons, kWheelBtnSlots[slot - 4].key, num);
            wheel_map_save_and_apply(std::wstring(items()[id].label) + L" -> " + wheel_slot_display(s_edit, slot) + L".");
            return true;
        }
    }
    return false;
}

/* ---- drawing ---- */

/* The clear parts of a popup or tag; push_image paints them black, then white. */
COLORREF kKey = RGB(255, 0, 255);
double s_paint_scale = 1;       /* device pixels per unit while painting */
double s_paint_ax = 1;          /* horizontal factor on top of it (paint_ax) */
double s_paint_ox = 0;          /* device x of the layout's left edge (a centred page) */

/* Ultrawide, anamorphic (RSX_ULTRAWIDE=1): the 16:9 backbuffer is stretched
 * over a 21:9 window, so overlays are painted 3/4 as wide to come out
 * undistorted. Native (=2) has a 21:9 backbuffer: no squeeze. */
double paint_ax()
{
    static double ax = 0;
    if (ax == 0) { const char* e = getenv("RSX_ULTRAWIDE"); ax = (e && e[0] == '1' && !e[1]) ? 0.75 : 1.0; }
    return ax;
}

COLORREF grey(int v) { return RGB(v, v, v); }

void fill(HDC dc, int x0, int y0, int x1, int y1, COLORREF c)
{
    RECT r = { x0, y0, x1, y1 };
    HBRUSH b = CreateSolidBrush(c); FillRect(dc, &r, b); DeleteObject(b);
}

void text(HDC dc, HFONT f, int x, int y, COLORREF c, const std::wstring& s, UINT align = TA_LEFT)
{
    HGDIOBJ o = SelectObject(dc, f);
    SetTextColor(dc, c); SetTextAlign(dc, align | TA_TOP);
    TextOutW(dc, x, y, s.c_str(), (int)s.size());
    SelectObject(dc, o);
}

HFONT font(int h, int weight)
{
    return CreateFontW(-h, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0,
                       L"Segoe UI");
}

/* Draw in device pixels for the duration of a shape: gradients and round
 * caps are filled row by row at full resolution. */
struct DeviceSpace {
    HDC dc; XFORM saved;
    explicit DeviceSpace(HDC d) : dc(d) {
        GetWorldTransform(dc, &saved);
        XFORM id = { 1, 0, 0, 1, 0, 0 }; SetWorldTransform(dc, &id);
    }
    ~DeviceSpace() { SetWorldTransform(dc, &saved); }
};

/* A vertical gradient over a rectangle, one device row at a time. */
template <class F> void vgrad(HDC dc, double x0, double y0, double x1, double y1, F colour)
{
    DeviceSpace ds(dc);
    const double s = s_paint_scale, sx = s * s_paint_ax;
    const int X0 = (int)(x0 * sx + s_paint_ox + 0.5), X1 = (int)(x1 * sx + s_paint_ox + 0.5);
    const int Y0 = (int)(y0 * s + 0.5), Y1 = (int)(y1 * s + 0.5);
    for (int y = Y0; y < Y1; y++) fill(dc, X0, y, X1, y + 1, colour((y + 0.5 - Y0) / (Y1 - Y0)));
}

/* A stadium (rectangle with half-circle ends), filled row by row, its
 * colour a function of the height through it. */
template <class F> void stadium(HDC dc, double x0, double y0, double x1, double y1, F colour)
{
    DeviceSpace ds(dc);
    const double s = s_paint_scale, ax = s_paint_ax;
    const double X0 = x0 * s * ax + s_paint_ox, X1 = x1 * s * ax + s_paint_ox, Y0 = y0 * s, Y1 = y1 * s, r = (Y1 - Y0) / 2, cy = Y0 + r;
    for (int y = (int)Y0; y < (int)(Y1 + 0.999); y++) {
        const double yc = y + 0.5, dy = yc - cy;
        if (dy * dy > r * r) continue;
        const double half = sqrt(r * r - dy * dy), rx = r * ax;   /* the ends are ellipses when squeezed */
        fill(dc, (int)(X0 + rx - half * ax + 0.5), y, (int)(X1 - rx + half * ax + 0.5), y + 1,
             colour((yc - Y0) / (Y1 - Y0)));
    }
}

int lerp(int a, int b, double t) { return (int)(a + (b - a) * (t < 0 ? 0 : t > 1 ? 1 : t)); }

/* The game's pill button: a grey edge, a black (or, focused, red) ring, and a
 * glass face: light grey-blue fading down to the middle, then near-black
 * rising again at the bottom edge. */
void pill(HDC dc, double x0, double y0, double x1, double y1, bool focused)
{
    stadium(dc, x0, y0, x1, y1, [](double) { return grey(46); });
    stadium(dc, x0 + 1, y0 + 1, x1 - 1, y1 - 1, [&](double) { return focused ? RGB(237, 0, 0) : grey(0); });
    stadium(dc, x0 + 3, y0 + 3, x1 - 3, y1 - 3, [](double t) {
        if (t < 0.5) { const double u = t / 0.5;
            return RGB(lerp(123, 57, u), lerp(125, 56, u), lerp(140, 57, u)); }
        const double u = (t - 0.5) / 0.5;
        const int v = u < 0.6 ? lerp(19, 3, u / 0.6) : lerp(3, 60, (u - 0.6) / 0.4);
        return grey(v);
    });
}

/* A triangle shaded between grey values at its corners, rasterised in device
 * pixels with 4x4 coverage at its edges, blended over what is there. */
void shaded_tri(HDC dc, const double (&p)[3][2], const int (&g)[3])
{
    DeviceSpace ds(dc);
    const double s = s_paint_scale;
    double X[3], Y[3];
    for (int k = 0; k < 3; k++) { X[k] = p[k][0] * s * s_paint_ax + s_paint_ox; Y[k] = p[k][1] * s; }
    const double area = (X[1] - X[0]) * (Y[2] - Y[0]) - (X[2] - X[0]) * (Y[1] - Y[0]);
    if (fabs(area) < 1e-6) return;
    auto bary = [&](double x, double y, double* w) {
        w[0] = ((X[1] - x) * (Y[2] - y) - (X[2] - x) * (Y[1] - y)) / area;
        w[1] = ((X[2] - x) * (Y[0] - y) - (X[0] - x) * (Y[2] - y)) / area;
        w[2] = 1 - w[0] - w[1];
    };
    const int x0 = (int)floor(fmin(X[0], fmin(X[1], X[2]))), x1 = (int)ceil(fmax(X[0], fmax(X[1], X[2])));
    const int y0 = (int)floor(fmin(Y[0], fmin(Y[1], Y[2]))), y1 = (int)ceil(fmax(Y[0], fmax(Y[1], Y[2])));
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            int in = 0;
            double w[3];
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    bary(x + (sx + 0.5) / 4, y + (sy + 0.5) / 4, w);
                    in += w[0] >= 0 && w[1] >= 0 && w[2] >= 0;
                }
            if (!in) continue;
            bary(x + 0.5, y + 0.5, w);
            for (double& v : w) v = v < 0 ? 0 : v;
            const double sum = w[0] + w[1] + w[2];
            const int v = (int)((w[0] * g[0] + w[1] * g[1] + w[2] * g[2]) / sum + 0.5);
            const double a = in / 16.0;
            const COLORREF old = GetPixel(dc, x, y);
            auto mix = [&](int o) { return (int)(o * (1 - a) + v * a + 0.5); };
            SetPixelV(dc, x, y, RGB(mix(GetRValue(old)), mix(GetGValue(old)), mix(GetBValue(old))));
        }
}

/* The game's arrow cursor (from the user's screenshot of it, ~28 x 42 pixels
 * at 1080p): a bevelled arrowhead with a tail on its left, tip at (x, y). Its
 * left face is dark with a lighter rim, its right face bright at the tip and
 * greying towards the notch, all inside a thin grey outline. */
void cursor(HDC dc, int x, int y)
{
    /* Tip, end of the left tail, the notch, the right wing. */
    const double T[2] = { x + 0.0, y + 0.0 }, A[2] = { x + 1.3, y + 28.0 },
                 N[2] = { x + 6.2, y + 20.4 }, R[2] = { x + 18.7, y + 20.6 };
    /* The outline: the same shape grown about its middle. */
    const double cx = x + 6.5, cy = y + 17.0;
    auto grow = [&](const double (&q)[2], double k, double* o) { o[0] = cx + (q[0] - cx) * k; o[1] = cy + (q[1] - cy) * k; };
    double t[2], a[2], n[2], r[2];
    /* A black edge outside the outline, so it reads over light lists. */
    grow(T, 1.17, t); grow(A, 1.17, a); grow(N, 1.17, n); grow(R, 1.17, r);
    t[1] -= 1.2;
    shaded_tri(dc, { { t[0], t[1] }, { a[0], a[1] }, { n[0], n[1] } }, { 0, 0, 0 });
    shaded_tri(dc, { { t[0], t[1] }, { n[0], n[1] }, { r[0], r[1] } }, { 0, 0, 0 });
    grow(T, 1.09, t); grow(A, 1.09, a); grow(N, 1.09, n); grow(R, 1.09, r);
    t[1] -= 0.6;
    shaded_tri(dc, { { t[0], t[1] }, { a[0], a[1] }, { n[0], n[1] } }, { 140, 140, 120 });
    shaded_tri(dc, { { t[0], t[1] }, { n[0], n[1] }, { r[0], r[1] } }, { 160, 120, 150 });
    shaded_tri(dc, { { T[0], T[1] }, { A[0], A[1] }, { N[0], N[1] } }, { 70, 115, 4 });       /* left face */
    shaded_tri(dc, { { T[0], T[1] }, { N[0], N[1] }, { R[0], R[1] } }, { 252, 90, 205 });     /* right face */
}

/* Text centred in a box, both ways (the button labels). */
void text_in(HDC dc, HFONT f, int x0, int y0, int x1, int y1, COLORREF c, const wchar_t* s, UINT fmt = DT_CENTER)
{
    HGDIOBJ o = SelectObject(dc, f);
    SetTextColor(dc, c); SetTextAlign(dc, TA_LEFT | TA_TOP);
    RECT r = { x0, y0, x1, y1 };
    DrawTextW(dc, s, -1, &r, fmt | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, o);
}

/* A pill button with its label. */
void button(HDC dc, int x0, int y, const wchar_t* label, bool focused)
{
    static HFONT f = font(21, FW_NORMAL);
    pill(dc, x0, y, x0 + kBtnW, y + kBtnH, focused);
    text_in(dc, f, x0, y, x0 + kBtnW, y + kBtnH - 2, grey(232), label);
}

/* The popup's frame: the scene dimmed around it (key colour), a black edge,
 * flat grey 28, and the dark header gradient at its top. */
void panel(HDC dc, int x0, int y0, int x1, int y1)
{
    fill(dc, 0, 0, kW, kH, kKey);
    fill(dc, x0 - 1, y0 - 1, x1 + 1, y1 + 1, grey(0));
    fill(dc, x0, y0, x1, y1, grey(28));
    vgrad(dc, x0, y0, x1, y0 + 26, [](double t) {
        return t < 0.04 ? RGB(45, 45, 48) : t < 0.1 ? grey(27) : t < 0.3 ? grey(lerp(9, 4, (t - 0.1) / 0.2))
                                                                          : grey(lerp(4, 28, (t - 0.3) / 0.7)); });
}

/* Where each page's name sits on the section bar (layout units), for clicks. */
struct SectionTab { int x0, x1; };
std::vector<SectionTab> s_tabs;

/* A section bar like the Options screen's "General", with the L1 and R1
 * knobs that switch pages; red rules when it is the selected row. Every
 * page is named on it, so the pages either side are visible: the current
 * one bright with a red underline, the others dimmed. A click on a name
 * opens that page. GT5P_SETTINGS_TABS=0: only the current page's name. */
void section_bar(HDC dc, int x0, int x1, int y, bool sel)
{
    static HFONT f = font(19, FW_SEMIBOLD), fk = font(9, FW_BOLD);
    static int tabs_on = -1;
    if (tabs_on < 0) { const char* e = getenv("GT5P_SETTINGS_TABS"); tabs_on = !(e && e[0] == '0'); }
    fill(dc, x0, y, x1, y + 1, sel ? RGB(168, 0, 0) : grey(70));
    vgrad(dc, x0, y + 1, x1, y + kBarH - 1, [](double t) { return grey(lerp(3, 34, t)); });
    fill(dc, x0, y + kBarH - 1, x1, y + kBarH, sel ? RGB(168, 0, 0) : grey(70));
    for (int k = 0; k < 2; k++) {
        const int cx = k ? x1 - 22 : x0 + 22, cy = y + kBarH / 2;
        stadium(dc, cx - 10, cy - 10, cx + 10, cy + 10, [](double t) { return grey(lerp(80, 18, t)); });
        stadium(dc, cx - 8.5, cy - 8.5, cx + 8.5, cy + 8.5, [](double t) { return grey(lerp(14, 44, t)); });
        text_in(dc, fk, cx - 10, cy - 10, cx + 10, cy + 10, grey(170), k ? L"R1" : L"L1");
    }
    const auto& secs = sections();
    std::vector<std::wstring> names;
    for (const wchar_t* t : secs) {
        std::wstring s = t;
        for (size_t i = 1; i < s.size(); i++) s[i] = (wchar_t)towlower(s[i]);   /* VIDEO -> Video */
        names.push_back(s);
    }
    s_tabs.assign(secs.size(), SectionTab{ 0, 0 });
    if (!tabs_on) {
        text_in(dc, f, x0, y, x1, y + kBarH - 1, grey(180), names[s_section].c_str());
        return;
    }
    /* The names share the room between the knobs, evenly spaced; the font
     * steps down if they would not fit (it is 15 units for ten pages). */
    const int a0 = x0 + 40, a1 = x1 - 40;
    static HFONT fonts[4][2];
    static const int sizes[4] = { 16, 15, 14, 12 };
    int pick = 3, total = 0;
    std::vector<int> widths(names.size());
    for (int k = 0; k < 4; k++) {
        if (!fonts[k][0]) { fonts[k][0] = font(sizes[k], FW_NORMAL); fonts[k][1] = font(sizes[k], FW_BOLD); }
        HGDIOBJ o = SelectObject(dc, fonts[k][1]);
        total = 0;
        for (size_t i = 0; i < names.size(); i++) {
            SIZE sz{}; GetTextExtentPoint32W(dc, names[i].c_str(), (int)names[i].size(), &sz);
            widths[i] = sz.cx; total += sz.cx;
        }
        SelectObject(dc, o);
        if (total + 10 * (int)names.size() <= a1 - a0) { pick = k; break; }
    }
    const double gap = names.empty() ? 0 : (double)(a1 - a0 - total) / (double)names.size();
    double x = a0 + gap / 2;
    for (size_t i = 0; i < names.size(); i++) {
        const int tx0 = (int)x, tx1 = (int)(x + widths[i]);
        const bool cur = (int)i == s_section;
        text_in(dc, fonts[pick][cur ? 1 : 0], tx0 - 4, y, tx1 + 4, y + kBarH - 2,
                cur ? grey(245) : grey(120), names[i].c_str());
        if (cur) fill(dc, tx0, y + kBarH - 6, tx1, y + kBarH - 4, RGB(200, 16, 16));
        s_tabs[i] = { (int)(x - gap / 2), (int)(x + widths[i] + gap / 2) };
        x += widths[i] + gap;
    }
}

/* The square drop-down button: black edge (red ring, selected), glass face,
 * white triangle. */
void drop_button(HDC dc, int x, int y, bool sel)
{
    const int s = kDropW;
    fill(dc, x, y, x + s, y + s, sel ? grey(66) : grey(5));
    fill(dc, x + 1, y + 1, x + s - 1, y + s - 1, sel ? RGB(168, 0, 0) : grey(0));
    vgrad(dc, x + 3, y + 3, x + s - 3, y + s - 3, [](double t) {
        if (t < 0.5) { const double u = t / 0.5; return RGB(lerp(123, 74, u), lerp(121, 73, u), lerp(132, 74, u)); }
        const double u = (t - 0.5) / 0.5;
        return grey(u < 0.6 ? lerp(10, 3, u / 0.6) : lerp(3, 38, (u - 0.6) / 0.4)); });
    POINT tri[] = { {x + s / 2 - 7, y + s / 2 - 3}, {x + s / 2 + 7, y + s / 2 - 3}, {x + s / 2, y + s / 2 + 5} };
    HBRUSH b = CreateSolidBrush(grey(231)); HPEN p = CreatePen(PS_SOLID, 1, grey(231));
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    Polygon(dc, tri, 3);
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(b); DeleteObject(p);
}

/* The open list, over the rows: light grey entries, the highlighted one a
 * pale blue gradient, a dash before the current value. Placed so the
 * current value sits on the value box. */
const int kDropItemH = 38;
int drop_top(int row_y, int cur, int n)
{
    int y = row_y - 3 - cur * kDropItemH;
    if (y + n * kDropItemH > kRule2) y = kRule2 - n * kDropItemH;
    if (y < kRule1) y = kRule1;
    return y;
}
void drop_list(HDC dc, const Item& it, int row_y)
{
    static HFONT f = font(19, FW_NORMAL);
    const int n = (int)it.choices.size(), cur = choice_index(it);
    const int y0 = drop_top(row_y, cur, n);
    const int x0 = kFieldX, x1 = kFieldX + kFieldW;
    fill(dc, x0 + 3, y0 + 3, x1 + 3, y0 + n * kDropItemH + 3, grey(14));   /* shadow */
    for (int i = 0; i < n; i++) {
        const int y = y0 + i * kDropItemH;
        if (i == s_drop_sel)
            vgrad(dc, x0, y, x1, y + kDropItemH, [](double t) {
                return RGB(lerp(154, 222, t), lerp(181, 244, t), lerp(206, 250, t)); });
        else fill(dc, x0, y, x1, y + kDropItemH, grey(204));
        fill(dc, x0, y + kDropItemH - 1, x1, y + kDropItemH, grey(127));
        if (i == cur) fill(dc, x0 + 13, y + kDropItemH / 2, x0 + 20, y + kDropItemH / 2 + 2, grey(115));
        text_in(dc, f, x0 + 30, y, x1 - 8, y + kDropItemH - 2, grey(51), it.choices[i].label, DT_LEFT);
    }
}

/* The page: one of the game's settings popups. */
void paint(HDC dc)
{
    static HFONT fTitle = font(22, FW_NORMAL), fRow = font(19, FW_NORMAL), fTag = font(12, FW_SEMIBOLD),
                 fHelp = font(15, FW_NORMAL), fStatus = font(13, FW_NORMAL);
    SetBkMode(dc, TRANSPARENT);
    panel(dc, kPx0, kPy0, kPx1, kPy1);
    text_in(dc, fTitle, kPx0, kPy0 + 4, kPx1, kPy0 + 34, grey(178), L"PC Settings");

    const std::vector<int> r = rows();
    const std::vector<Slot> sl = layout(r);
    if (s_sel >= (int)r.size()) s_sel = (int)r.size() - 1;
    fill(dc, kPx0, kRule1, kPx1, kRule1 + 1, grey(76));
    fill(dc, kPx0, kRule2, kPx1, kRule2 + 1, grey(76));
    int drop_row = -1;
    for (size_t i = 0; i < r.size(); i++) {
        const int id = r[i], y = sl[i].y;
        const bool sel = (int)i == s_sel;
        if (id == kRowHead) { section_bar(dc, sl[i].x0, sl[i].x1, y, sel); continue; }
        if (id < 0) {
            button(dc, sl[i].x0, y, id == kRowRestart ? L"Restart" : id == kRowDefaults ? L"Defaults" : id == kRowSetup211 ? L"Set up 2.11" : id == kRowMods ? L"Mods" : L"Close", sel);
            if (sel) cursor(dc, sl[i].x0 + kBtnW / 2 + 2, y + 18);
            continue;
        }
        const Item& it = items()[id];
        const bool over = !launcher_item(it) && gt5p_settings_overridden(it.env);
        if (it.key >= 0 || it.wmap >= 0) {
            /* A binding: the button's name, then its key or wheel axis/button in a box. */
            const int x0 = sl[i].x0, fx = x0 + 160;
            const int rh = it.wmap >= 0 ? 28 : kKeyH;
            const bool cap = (it.key >= 0 && s_capture == id) || (it.wmap >= 0 && s_wcap == id);
            text_in(dc, fRow, x0, y, fx - 16, y + rh - 4, over ? grey(100) : grey(190), it.label, DT_RIGHT);
            fill(dc, fx - 1, y - 1, fx + kKeyFieldW + 1, y + rh - 3, sel ? RGB(168, 0, 0) : grey(5));
            fill(dc, fx, y, fx + kKeyFieldW, y + rh - 4, RGB(0, 4, 0));
            std::wstring v;
            if (it.key >= 0) {
                const char* kn = gt5p_key_name(it.get(s_edit));
                v = cap ? L"Press a key" : kn ? widen(kn) : L"?";
            } else {
                v = cap ? (it.wmap == 0 ? L"Turn wheel" : it.wmap <= 3 ? L"Press pedal" : L"Press button")
                        : wheel_slot_display(s_edit, it.wmap);
            }
            text_in(dc, fRow, fx + 12, y, fx + kKeyFieldW - 6, y + rh - 5,
                    cap ? RGB(237, 60, 60) : over ? grey(100) : grey(204), v.c_str(), DT_LEFT | DT_END_ELLIPSIS);
            if (sel && !cap) cursor(dc, fx + kKeyFieldW - 18, y + (rh > 30 ? 8 : 5));
            continue;
        }
        const bool later = !it.apply && !over && it.get(s_edit) != it.get(s_boot);
        /* A dotted rule under every row but the last. */
        if (i + 1 < r.size() && r[i + 1] >= 0)
            for (int x = kPx0 + 26; x < kPx1 - 26; x += 7)
                fill(dc, x, y + kFieldH + 11, x + 2, y + kFieldH + 12, grey(96));
        text_in(dc, fRow, kPx0 + 20, y, kFieldX - 24, y + kFieldH - 2, over ? grey(100) : grey(190), it.label, DT_RIGHT);
        fill(dc, kFieldX, y, kFieldX + kFieldW, y + kFieldH, RGB(0, 4, 0));
        fill(dc, kFieldX, y + kFieldH - 1, kFieldX + kFieldW, y + kFieldH, RGB(99, 101, 107));
        text_in(dc, fRow, kFieldX + 14, y, kFieldX + kFieldW - 8, y + kFieldH - 2, over ? grey(100) : grey(204),
                (it.env && !strcmp(it.env,"PAD_MODERN_CONTROLS") && gt5p_modern_controls_state()<0) ?
                    (gt5p_modern_controls_state()==-2 ? L"Unavailable" : L"Waiting for game") : it.choices[choice_index(it)].label, DT_LEFT | DT_END_ELLIPSIS);
        /* Notes sit in the row's empty left side. */
        if (later)
            text_in(dc, fTag, kPx0 + 30, y, kFieldX, y + kFieldH - 2, RGB(237, 60, 60), L"after restart", DT_LEFT);
        else if (over)
            text_in(dc, fTag, kPx0 + 30, y, kFieldX, y + kFieldH - 2, grey(110),
                    (L"set by " + widen(it.env)).c_str(), DT_LEFT);
        drop_button(dc, kFieldX + kFieldW + 12, y, sel);
        if (sel && !s_drop) cursor(dc, kFieldX + kFieldW + 12 + kDropW / 2 + 1, y + kDropW / 2 + 2);
        if (sel && s_drop) drop_row = (int)i;
    }

    /* The selected item's help, plus live wheel telemetry on WHEEL / WHEEL MAP. */
    std::wstring wheel_line;
    if (wheel_page() || wheel_map_page()) {
        WheelRawUi raw = {};
        if (wheel_host_raw(&raw) && raw.connected) {
            char buf[320];
            int held_btn = 0;
            for (int b = 0; b < 32; b++) if (raw.buttons & (1u << b)) { held_btn = b + 1; break; }
            if (raw.hint[0])
                snprintf(buf, sizeof buf, "Wheel: %s (%04X:%04X) -- %s", raw.name, raw.vid, raw.pid, raw.hint);
            else if (held_btn)
                snprintf(buf, sizeof buf, "Wheel: %s (%04X:%04X%s)  X:%.2f Y:%.2f RZ:%.2f S0:%.2f  Btn %d",
                         raw.name, raw.vid, raw.pid, raw.ffb ? " FFB" : "",
                         raw.axis[0], raw.axis[1], raw.axis[5], raw.axis[6], held_btn);
            else
                snprintf(buf, sizeof buf, "Wheel: %s (%04X:%04X%s)  X:%.2f Y:%.2f RZ:%.2f S0:%.2f",
                         raw.name, raw.vid, raw.pid, raw.ffb ? " FFB" : "",
                         raw.axis[0], raw.axis[1], raw.axis[5], raw.axis[6]);
            wheel_line = widen(buf);
        } else if (raw.hint[0]) {
            wheel_line = L"Wheel: " + widen(raw.hint);
        } else {
            wheel_line = L"Wheel: none detected -- plug in your wheel and ensure its PC software (e.g. Logitech G HUB) is running.";
        }
    }
    if (s_sel >= 0 && s_sel < (int)r.size() && r[s_sel] >= 0) {
        RECT rc = { kPx0 + 40, kHelpY, kPx1 - 40, kRule2 - 6 };
        HGDIOBJ o = SelectObject(dc, fHelp);
        SetTextColor(dc, grey(150)); SetTextAlign(dc, TA_LEFT | TA_TOP);
        std::wstring help = items()[r[s_sel]].help;
        if (s_capture >= 0)
            help = std::wstring(L"Press the key for ") + items()[s_capture].label +
                   L". Escape (or Circle) cancels.";
        else if (s_wcap >= 0) {
            const int ws = items()[s_wcap].wmap;
            help = (ws == 0 ? L"Turn your steering wheel to bind " : ws <= 3 ? L"Press the pedal to bind " : L"Press a wheel button to bind ")
                 + std::wstring(items()[s_wcap].label) + L". Escape (or Circle) cancels.";
        }
        if (!wheel_line.empty() && s_capture < 0)
            help = wheel_line + L"\n" + help;
        DrawTextW(dc, help.c_str(), -1, &rc, DT_WORDBREAK | DT_NOPREFIX | DT_CENTER);
        SelectObject(dc, o);
    } else if (s_sel == 0) {
        text_in(dc, fHelp, kPx0, kHelpY, kPx1, kHelpY + 24, grey(150), L"L1 / R1, or Left / Right here: the other settings pages");
    } else if (!wheel_line.empty()) {
        text_in(dc, fHelp, kPx0 + 40, kHelpY, kPx1 - 40, kRule2 - 6, grey(150), wheel_line.c_str());
    }
    if (!s_status.empty()) text_in(dc, fStatus, kPx0, kBtnY + kBtnH + 4, kPx1, kPy1 - 2, grey(130), s_status.c_str());
    if (drop_row >= 0) {
        const Item& it = items()[r[drop_row]];
        drop_list(dc, it, sl[drop_row].y);
        const int y0 = drop_top(sl[drop_row].y, choice_index(it), (int)it.choices.size());
        cursor(dc, kFieldX + kFieldW - 70, y0 + s_drop_sel * kDropItemH + 18);
    }
}

/* The quit question, a copy of the game's "Confirmation" popup. */
const int kQx0 = 360, kQx1 = 920, kQy0 = 175, kQy1 = 547, kQbtnY = 480;
const int kQbtnX[2] = { 475, 648 };
void paint_quit(HDC dc)
{
    static HFONT fTitle = font(22, FW_NORMAL), fMsg = font(22, FW_NORMAL);
    SetBkMode(dc, TRANSPARENT);
    panel(dc, kQx0, kQy0, kQx1, kQy1);
    text_in(dc, fTitle, kQx0, kQy0 + 4, kQx1, kQy0 + 34, grey(178), L"Confirmation");
    text_in(dc, fMsg, kQx0, 300, kQx1, 342, grey(236), L"Quit Gran Turismo 5?");
    fill(dc, kQx0, 448, kQx1, 449, grey(76));
    button(dc, kQbtnX[0], kQbtnY, L"Cancel", s_quit_sel == 0);
    button(dc, kQbtnX[1], kQbtnY, L"OK", s_quit_sel == 1);
    cursor(dc, kQbtnX[s_quit_sel] + kBtnW / 2 + 2, kQbtnY + 18);
}

void activate(int dir);
void press();
void switch_section(int dir);
void drop_pick();

/* A click at layout position (mx, my). With the list open: pick the entry
 * under it, or close the list. Otherwise: the section bar's knobs switch
 * pages, a value box or drop-down button opens its list, a button presses. */
void click(int mx, int my)
{
    const std::vector<int> r = rows();
    const std::vector<Slot> sl = layout(r);
    if (s_drop) {
        if (s_sel >= 0 && s_sel < (int)r.size() && r[s_sel] >= 0) {
            const Item& it = items()[r[s_sel]];
            const int n = (int)it.choices.size(), y0 = drop_top(sl[s_sel].y, choice_index(it), n);
            if (mx >= kFieldX && mx < kFieldX + kFieldW && my >= y0 && my < y0 + n * kDropItemH) {
                s_drop_sel = (my - y0) / kDropItemH;
                drop_pick();
                return;
            }
        }
        s_drop = false;
        return;
    }
    for (size_t i = 0; i < r.size(); i++) {
        const int id = r[i], y = sl[i].y;
        if (id == kRowHead) {
            if (my >= y && my < y + kBarH && mx >= sl[i].x0 && mx < sl[i].x1) {
                s_sel = 0;
                if (mx < sl[i].x0 + 40) switch_section(-1);
                else if (mx >= sl[i].x1 - 40) switch_section(+1);
                else for (size_t t = 0; t < s_tabs.size(); t++)
                    if (mx >= s_tabs[t].x0 && mx < s_tabs[t].x1 && (int)t != s_section) {
                        switch_section((int)t - s_section);   /* straight to that page */
                        break;
                    }
                return;
            }
        } else if (id < 0) {
            if (my >= y && my < y + kBtnH && mx >= sl[i].x0 && mx < sl[i].x1) { s_sel = (int)i; press(); return; }
        } else if (items()[id].key >= 0 || items()[id].wmap >= 0) {
            const int rh = items()[id].wmap >= 0 ? 28 : kKeyH;
            if (my >= y && my < y + rh - 3 && mx >= sl[i].x0 && mx < sl[i].x1) { s_sel = (int)i; press(); return; }
        } else if (my >= y && my < y + kFieldH && mx >= kPx0 && mx < kFieldX + kFieldW + 12 + kDropW) {
            s_sel = (int)i;
            if (mx >= kFieldX) press();
            return;
        }
    }
}

/* Overlay: paint at the backbuffer's scale and hand the backend premultiplied
 * BGRA: full screen, or a tag in the corner. Painted twice, its clear parts
 * (kKey) black and then white: the black pass is the premultiplied colour,
 * and the difference between the passes is how much shows through, exact at
 * antialiased text edges. The clear parts then get black at `dim` alpha. */
void push_image(int lw, int lh, void (*painter)(HDC), bool corner, int dim)
{
    int bw = 0, bh = 0;
    rsx_d3d12_backbuffer_size(&bw, &bh);
    if (bw <= 0 || bh <= 0) return;
    double sc = bh / 720.0;
    const double ax = paint_ax();
    if (lw * sc * ax > bw) sc = bw / (double)lw / ax;
    /* A full-screen page in ultrawide spans the whole backbuffer, its layout
     * centred, so the dimmed scene around it reaches the window's sides. */
    const double ox = (!corner && lw * sc * ax < bw - 1) ? (bw - lw * sc * ax) * 0.5 : 0.0;
    const int w = ox > 0 ? bw : (int)(lw * sc * ax + 0.5), h = (int)(lh * sc + 0.5);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* pass[2] = { nullptr, nullptr };
    HBITMAP bm[2] = { NULL, NULL };
    HDC dc = CreateCompatibleDC(NULL);
    for (int k = 0; k < 2 && dc; k++) bm[k] = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &pass[k], NULL, 0);
    if (!bm[0] || !bm[1]) {
        for (HBITMAP b : bm) if (b) DeleteObject(b);
        if (dc) DeleteDC(dc);
        return;
    }
    HGDIOBJ o = SelectObject(dc, bm[0]);
    SetGraphicsMode(dc, GM_ADVANCED);
    XFORM x = { (FLOAT)(sc * ax), 0, 0, (FLOAT)sc, (FLOAT)ox, 0 };
    SetWorldTransform(dc, &x);
    s_paint_scale = sc; s_paint_ax = ax; s_paint_ox = ox;
    for (int k = 0; k < 2; k++) {
        SelectObject(dc, bm[k]);
        kKey = k ? RGB(255, 255, 255) : RGB(0, 0, 0);
        if (ox > 0) { DeviceSpace ds(dc); fill(dc, 0, 0, w, h, kKey); }
        painter(dc);
    }
    kKey = RGB(255, 0, 255);
    s_paint_scale = 1; s_paint_ax = 1; s_paint_ox = 0;
    GdiFlush();
    uint32_t* p = (uint32_t*)pass[0];
    const uint32_t* q = (const uint32_t*)pass[1];
    for (int i = 0, n = w * h; i < n; i++) {
        const uint32_t c = p[i] & 0xFFFFFFu;
        const int through = (int)((q[i] >> 8) & 255) - (int)((c >> 8) & 255);   /* green channel */
        const uint32_t a = (uint32_t)(255 - (through < 0 ? 0 : through));
        /* over black at `dim`: colour stays, alpha a + (1 - a) * dim */
        const uint32_t out_a = a + (255 - a) * (uint32_t)dim / 255;
        p[i] = (out_a << 24) | c;
    }
    SelectObject(dc, o);
    DeleteObject(bm[1]);
    void* bits = pass[0];
    if (corner) {   /* bottom right, in the game's own margin */
        /* In ultrawide, the corner of the title's centred 16:9 area. */
        const double area = bh * 16.0 / 9.0 * ax;
        const int right = area < bw ? (int)((bw + area) * 0.5) : bw;
        rsx_d3d12_overlay_set(bits, w, h, right - w - (int)(40 * sc * ax), bh - h - (int)(34 * sc));
    } else {
        s_scale = sc; s_scale_x = sc * ax; s_oy = (bh - h) / 2;
        rsx_d3d12_overlay_set(bits, w, h, (bw - w) / 2, s_oy);
        s_ox = (bw - w) / 2 + (int)ox;   /* the layout's left edge, for the mouse */
    }
    DeleteObject(bm[0]); DeleteDC(dc);
}

/* GT5P_SETTINGS_UI_ALPHA: how far the scene around the popups is dimmed,
 * 0-255 (default 186: the game's own popups leave it at about 27%). */
int dim_alpha()
{
    static int a = -1;
    if (a < 0) { const char* e = getenv("GT5P_SETTINGS_UI_ALPHA"); a = e ? atoi(e) & 255 : 186; }
    return a;
}

void render_overlay()
{
    if (s_quit) push_image(kW, kH, paint_quit, false, dim_alpha());
    else if (s_open && s_overlay) push_image(kW, kH, paint, false, dim_alpha());
}

/* The game's Options screen: a tag in its bottom right saying the port's
 * settings are a button away. GT5P_OPTIONS_HINT=0: no tag, and Select does
 * not open the page. */
const int kHintW = 200, kHintH = 37;
void paint_hint(HDC dc)
{
    static HFONT fBtn = font(16, FW_SEMIBOLD), fLbl = font(18, FW_NORMAL);
    SetBkMode(dc, TRANSPARENT);
    fill(dc, 0, 0, kHintW, kHintH, kKey);
    text(dc, fLbl, 92, 8, grey(212), L"PC settings");
    /* The Select button of the device in use (gt5p_glyphs.h): Create, Share,
     * View, minus or the bound key; a PS3 pad keeps the "SELECT" pill. */
    const double sc = s_paint_scale;
    const int hp = (int)(29 * sc + 0.5);
    std::vector<unsigned char> px;
    const int iw = gt5p_glyphs_select_icon(hp, (int)(82 * sc), px);
    if (!iw) {
        pill(dc, 0, 4, 82, kHintH - 4, false);
        text(dc, fBtn, 41, 9, grey(232), L"SELECT", TA_CENTER);
        return;
    }
    DeviceSpace ds(dc);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = iw; bi.bmiHeader.biHeight = -hp;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC mdc = CreateCompatibleDC(dc);
    HBITMAP bm = mdc ? CreateDIBSection(mdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0) : NULL;
    if (bm) {
        memcpy(bits, px.data(), px.size());
        HGDIOBJ o = SelectObject(mdc, bm);
        BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        const int dw = (int)(iw * s_paint_ax + 0.5);   /* squeezed with the rest in ultrawide */
        AlphaBlend(dc, (int)((92 - 10) * sc * s_paint_ax + 0.5) - dw, (int)(4 * sc + 0.5), dw, hp, mdc, 0, 0, iw, hp, bf);
        SelectObject(mdc, o);
        DeleteObject(bm);
    }
    if (mdc) DeleteDC(mdc);
}

bool hint_enabled()
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("GT5P_OPTIONS_HINT"); on = !(e && e[0] == '0'); }
    return on != 0;
}

volatile int s_in_options;     /* the title's Options screen is up */
volatile int s_in_race;        /* a race is loaded (RaceRoot until another screen) */
bool s_hint_shown;

/* Which screen the title is on, from the screen scripts (.gpb) it reads.
 * GT5P_SETTINGS_UI_DBG=1 prints each one. */
volatile int s_dbg_screens = -1;
int s_opt_word_seen = 0;
/* A title without screen scripts (Gran Turismo 5) names its screens by
 * measured files instead: cellPad passes their labels (PAD_MILESTONE_FILES
 * "<label>=<path tail>") here too. GT5P_SCREEN_ROLES gives each label a role:
 * "<label>:race" starts a race (the page pauses it), "<label>:options" is the
 * Options screen (the hint), "<label>:screen" is any other screen, which ends
 * both. Set per executable from config/targets.json (screen_roles). Returns 1
 * when the label was listed. */
int screen_role_seen(const char* b)
{
    static char list[512]; static int init;
    if (!init) { init = 1; const char* e = getenv("GT5P_SCREEN_ROLES"); if (e) snprintf(list, sizeof list, "%s", e); }
    const size_t n = strlen(b);
    for (const char* p = list; *p; ) {
        const char* c = strchr(p, ',');
        const size_t len = c ? (size_t)(c - p) : strlen(p);
        if (len > n + 1 && p[n] == ':' && !strncmp(p, b, n)) {
            const char* role = p + n + 1; const size_t rl = len - n - 1;
            const int race = rl == 4 && !strncmp(role, "race", 4);
            const int options = rl == 7 && !strncmp(role, "options", 7);
            s_in_race = race; s_in_options = options;
            if (options) s_opt_word_seen = 0;
            if (s_dbg_screens > 0) fprintf(stderr, "[settings-ui] %llu ms screen label %s (%.*s)%c",
                                           GetTickCount64(), b, (int)rl, role, 10);
            return 1;
        }
        if (!c) break;
        p = c + 1;
    }
    return 0;
}
/* GT5P_TOP_MENU_WORD=<addr>=<value> (hex): a guest word that holds that
 * value while the title's top menu is up. Gran Turismo 5 reads no file when
 * it returns to the top menu (from Options or a race's exit), so no label
 * can end those screens; this word does (top_menu_word in targets.json,
 * found by GT5P_MEMSNAP and confirmed with GT5P_WATCH). Checked each UI tick. */
extern "C" uint8_t* vm_base;
void top_menu_poll()
{
    static int on = -1; static uint32_t addr, value;
    if (on < 0) {
        const char* e = getenv("GT5P_TOP_MENU_WORD");
        unsigned a = 0, v = 0;
        on = e && vm_base && sscanf(e, "%x=%x", &a, &v) == 2 && a;
        addr = a; value = v;
    }
    if (on && (s_in_options || s_in_race)) {
        uint32_t w; memcpy(&w, vm_base + addr, 4);
        if (_byteswap_ulong(w) == value) {
            if (s_dbg_screens > 0) fprintf(stderr, "[settings-ui] %llu ms top menu (word %08X)%c", GetTickCount64(), addr, 10);
            s_in_options = 0; s_in_race = 0; s_opt_word_seen = 0;
        }
    }
    /* GT5P_OPTIONS_WORD=<addr>=<value> (hex): a guest word that holds <value>
     * while Options is open and changes away on leaving Options to ANY menu
     * (including GT Mode, where GT5P_TOP_MENU_WORD does not fire; issue #79).
     * Never clears s_in_race. */
    static int opt_on = -1; static uint32_t opt_addr, opt_val;
    if (opt_on < 0) {
        const char* e = getenv("GT5P_OPTIONS_WORD");
        unsigned a = 0, v = 0;
        opt_on = e && vm_base && sscanf(e, "%x=%x", &a, &v) == 2 && a;
        opt_addr = a; opt_val = v;
    }
    if (opt_on && s_in_options) {
        uint32_t w; memcpy(&w, vm_base + opt_addr, 4);
        if (_byteswap_ulong(w) == opt_val) {
            s_opt_word_seen = 1;
        } else if (s_opt_word_seen) {
            if (s_dbg_screens > 0) fprintf(stderr, "[settings-ui] %llu ms left options (word %08X=%X)%c", GetTickCount64(), opt_addr, _byteswap_ulong(w), 10);
            s_in_options = 0; s_opt_word_seen = 0;
        }
    }
}
void screen_seen(const char* b)
{
    const size_t n = strlen(b);
    if (s_dbg_screens < 0) { const char* e = getenv("GT5P_SETTINGS_UI_DBG"); s_dbg_screens = e && e[0] == '1'; }
    if (screen_role_seen(b)) return;
    if (n < 5 || strcmp(b + n - 4, ".gpb")) return;
    if (s_dbg_screens) fprintf(stderr, "[settings-ui] %llu ms screen %s\n", GetTickCount64(), b);
    /* Entering Options reads OptionRoot (then the gadget layer, DLSGadgetRoot);
     * leaving it reads the top menu's scripts again (TopRoot,
     * TopMenuTemplate); any other screen root also means it has gone. */
    if (!strcmp(b, "OptionRoot.gpb")) s_in_options = 1;
    else if (strcmp(b, "DLSGadgetRoot.gpb") &&
             (strstr(b, "Root.gpb") || strstr(b, "Template.gpb"))) s_in_options = 0;
    /* A race reads RaceRoot with its layers (OnboardMeterRoot, the mode's
     * QuickArcadeRoot, DLSGadgetRoot); any other screen root ends it. A mode
     * whose race root is not listed just never counts as racing. */
    if (!strcmp(b, "RaceRoot.gpb")) s_in_race = 1;
    else if (strcmp(b, "DLSGadgetRoot.gpb") && strcmp(b, "OnboardMeterRoot.gpb") &&
             strcmp(b, "QuickArcadeRoot.gpb") &&
             (strstr(b, "Root.gpb") || strstr(b, "Template.gpb"))) s_in_race = 0;
}

/* Escape in the game window (rsx_d3d12_backend.c calls this on its window
 * thread): ask first, unless GT5P_ESC_CONFIRM=0 or the page cannot be drawn
 * into the picture. Returns 1 when it will ask (the window then stays open). */
int esc_pressed(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("GT5P_ESC_CONFIRM"); on = !(e && e[0] == '0'); }
    if (!on || legacy_ui_flag() || !rsx_d3d12_overlay_available()) return 0;
    InterlockedExchange(&s_quit_req, 1);
    return 1;
}

/* Redraw whichever form is open. */
void redraw()
{
    if (s_quit) { render_overlay(); return; }
    if (!s_open) return;
    if (s_overlay) render_overlay();
    else InvalidateRect(s_wnd, NULL, FALSE);
}

/* ---- memory snapshots (a diagnostic) ----
 *
 * GT5P_MEMSNAP=<milestone>+<ms>[,...]: save every committed page of guest
 * memory once at each of those times, and on F9 over the game, to
 * scratch/memsnap/snapNN.bin as records of [u32 guest address, u32 length,
 * bytes]. Comparing snapshots taken in two states (paused and running, say)
 * finds the variable that tells them apart (scripts/diag/memsnap_diff.py).
 * The files hold game memory: they stay in the gitignored scratch folder. */
// vm_base is declared above top_menu_poll().
extern "C" long long pad_milestone_age_ms(const char* name);

void memsnap_write()
{
    static int n = 0;
    CreateDirectoryA("scratch", NULL);
    CreateDirectoryA("scratch\\memsnap", NULL);
    char path[96];
    snprintf(path, sizeof path, "scratch\\memsnap\\snap%02d.bin", n++);
    FILE* f = fopen(path, "wb");
    if (!f || !vm_base) { if (f) fclose(f); return; }
    unsigned long long total = 0;
    for (unsigned long long a = 0; a < 0x100000000ull;) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(vm_base + a, &mi, sizeof mi)) break;
        const unsigned long long end = (unsigned long long)((uint8_t*)mi.BaseAddress + mi.RegionSize - vm_base);
        if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
            for (unsigned long long p = a; p < end && p < 0x100000000ull; p += 0x1000000) {
                const uint32_t len = (uint32_t)((end - p) < 0x1000000 ? (end - p) : 0x1000000);
                const uint32_t ga = (uint32_t)p;
                fwrite(&ga, 4, 1, f); fwrite(&len, 4, 1, f); fwrite(vm_base + p, 1, len, f);
                total += len;
            }
        }
        a = end;
    }
    fclose(f);
    fprintf(stderr, "[memsnap] %s: %llu MB at %llu ms\n", path, total >> 20, GetTickCount64());
}

void memsnap_poll()
{
    static int on = -1;
    static std::vector<std::pair<std::string, long long>> at;
    static std::vector<bool> done;
    static bool f9_was;
    if (on < 0) {
        const char* e = getenv("GT5P_MEMSNAP");
        on = e && *e;
        if (on) {
            std::string s = e;
            size_t i = 0;
            while (i < s.size()) {
                size_t j = s.find(',', i); if (j == std::string::npos) j = s.size();
                std::string tok = s.substr(i, j - i);
                size_t plus = tok.find('+');
                if (plus != std::string::npos) at.push_back({ tok.substr(0, plus), atoll(tok.c_str() + plus + 1) });
                i = j + 1;
            }
            done.assign(at.size(), false);
        }
    }
    if (!on) return;
    for (size_t k = 0; k < at.size(); k++)
        if (!done[k] && pad_milestone_age_ms(at[k].first.c_str()) >= at[k].second) { done[k] = true; memsnap_write(); }
    const bool f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    if (f9 && !f9_was && GetForegroundWindow() == s_game) memsnap_write();
    f9_was = f9;
}

/* GT5P_SETTINGS_TOGGLE_AT=<milestone>+<ms>[,...]: open or close the page at
 * those times, for scripted runs (F1 and the PS button cannot be scripted). */
void set_open(bool open);
void toggle_poll()
{
    static int on = -1;
    static std::vector<std::pair<std::string, long long>> at;
    static std::vector<bool> done;
    if (on < 0) {
        const char* e = getenv("GT5P_SETTINGS_TOGGLE_AT");
        on = e && *e;
        for (std::string sv = on ? e : ""; !sv.empty();) {
            size_t j = sv.find(','); std::string tok = sv.substr(0, j);
            size_t plus = tok.find('+');
            if (plus != std::string::npos) at.push_back({ tok.substr(0, plus), atoll(tok.c_str() + plus + 1) });
            sv = j == std::string::npos ? "" : sv.substr(j + 1);
        }
        done.assign(at.size(), false);
    }
    if (!on) return;
    for (size_t k = 0; k < at.size(); k++)
        if (!done[k] && pad_milestone_age_ms(at[k].first.c_str()) >= at[k].second) { done[k] = true; set_open(!s_open); }
}

/* Game speed (issue #13, gt5p_present.cpp): F2 in the game window cycles
 * 1x, 2x, 3x, the vblank rate times that. GT5P_GAME_SPEED=0 turns the key
 * off. GT5P_GAME_SPEED_AT=<milestone>+<ms>:<factor>[,...] sets it at those
 * times, for scripted runs (menus must stay at 1x for timed presses). */
extern "C" volatile LONG g_gt5p_game_speed;
static void set_game_speed(LONG speed)
{
    if (speed < 1 || speed > 3 || speed == g_gt5p_game_speed) return;
    InterlockedExchange(&g_gt5p_game_speed, speed);
    fprintf(stderr, "[game-speed] %ldx\n", (long)speed);
}
void game_speed_poll(bool game_fg)
{
    static int on = -1, f2_was = 0;
    struct At { std::string ms; long long after; LONG speed; bool done; };
    static std::vector<At> at;
    if (on < 0) {
        const char* e = getenv("GT5P_GAME_SPEED");
        on = !(e && e[0] == '0');
        const char* a = getenv("GT5P_GAME_SPEED_AT");
        for (std::string sv = a ? a : ""; !sv.empty();) {
            size_t j = sv.find(','); std::string tok = sv.substr(0, j);
            size_t plus = tok.find('+'), colon = tok.find(':');
            if (plus != std::string::npos && colon != std::string::npos && colon > plus)
                at.push_back({ tok.substr(0, plus), atoll(tok.substr(plus + 1, colon - plus - 1).c_str()),
                               (LONG)atoi(tok.c_str() + colon + 1), false });
            sv = j == std::string::npos ? "" : sv.substr(j + 1);
        }
    }
    for (auto& t : at)
        if (!t.done && pad_milestone_age_ms(t.ms.c_str()) >= t.after) { t.done = true; set_game_speed(t.speed); }
    if (!on) return;
    const int f2 = (GetAsyncKeyState(VK_F2) & 0x8000) != 0;
    if (f2 && !f2_was && game_fg && !s_open && !s_quit) set_game_speed(g_gt5p_game_speed % 3 + 1);
    f2_was = f2;
}

/* GT5P_WATCH=addr[,addr...]: print each guest word there whenever it
 * changes (checked every UI tick, ~16 ms), to confirm a candidate found by
 * memsnap_diff.py. addr>off follows a pointer: the word at (word at addr)+off. */
uint32_t guest_word(uint32_t a)
{
    uint32_t w;
    memcpy(&w, vm_base + a, 4);
    return _byteswap_ulong(w);
}

/* GT5P_POKE=<milestone>+<ms>:addr=value[,addr=value...]: write guest words
 * (hex, big-endian as the guest reads them) once that long after the screen
 * opened; "!addr" rewrites that one on every poll after. A diagnostic, for
 * testing what a variable does (Phase 121's aspect ratio). */
void poke_poll()
{
    static int on = -1;
    static std::string ms_name; static long long ms_at;
    static std::vector<uint32_t> addr, val;
    static std::vector<bool> keep;
    static bool done;
    if (on < 0) {
        const char* e = getenv("GT5P_POKE");
        on = e && *e && vm_base;
        if (on) {
            std::string s = e;
            const size_t plus = s.find('+'), colon = s.find(':');
            if (plus == std::string::npos || colon == std::string::npos) { on = 0; return; }
            ms_name = s.substr(0, plus); ms_at = atoll(s.c_str() + plus + 1);
            for (size_t i = colon + 1; i < s.size();) {
                size_t j = s.find(',', i); if (j == std::string::npos) j = s.size();
                std::string tok = s.substr(i, j - i);
                const bool k = !tok.empty() && tok[0] == '!';
                if (k) tok.erase(0, 1);
                const size_t eq = tok.find('=');
                if (eq != std::string::npos) {
                    addr.push_back((uint32_t)strtoul(tok.c_str(), nullptr, 16));
                    val.push_back((uint32_t)strtoul(tok.c_str() + eq + 1, nullptr, 16));
                    keep.push_back(k);
                }
                i = j + 1;
            }
        }
    }
    if (!on || pad_milestone_age_ms(ms_name.c_str()) < ms_at) return;
    for (size_t k = 0; k < addr.size(); k++) {
        if (done && !keep[k]) continue;
        const uint32_t be = _byteswap_ulong(val[k]);
        memcpy(vm_base + addr[k], &be, 4);
        if (!done) fprintf(stderr, "[poke] %llu ms %08X = %08X\n", GetTickCount64(), addr[k], val[k]);
    }
    done = true;
}

void watch_poll()
{
    static int on = -1;
    /* addr>off: the word at (the word at addr) + off, when that is mapped. */
    static std::vector<uint32_t> addr, offs, last;
    static std::vector<bool> deref;
    if (on < 0) {
        const char* e = getenv("GT5P_WATCH");
        on = e && *e && vm_base;
        for (const char* p = e; on && p && *p;) {
            char* q = nullptr;
            addr.push_back((uint32_t)strtoul(p, &q, 16));
            deref.push_back(q && *q == '>');
            offs.push_back(q && *q == '>' ? (uint32_t)strtoul(q + 1, nullptr, 16) : 0);
            last.push_back(0xDEADBEEF);
            p = strchr(p, ','); if (p) p++;
        }
    }
    if (!on) return;
    for (size_t k = 0; k < addr.size(); k++) {
        uint32_t a = addr[k];
        if (deref[k]) {
            const uint32_t b = guest_word(a);
            if (!b || b >= 0xF0000000u) continue;
            a = b + offs[k];
        }
        const uint32_t v = guest_word(a);
        if (v != last[k]) {
            if (deref[k]) fprintf(stderr, "[watch] %llu ms %08X>%X (%08X) = %08X\n", GetTickCount64(), addr[k], offs[k], a, v);
            else fprintf(stderr, "[watch] %llu ms %08X = %08X\n", GetTickCount64(), addr[k], v);
            last[k] = v;
        }
    }
}


/* ---- window ---- */

/* Centre the page over the game. Only when it moved, and asynchronously:
 * the game window belonged to the present thread (with RSX_WINDOW_THREAD=0
 * it still does), and a synchronous
 * SetWindowPos every tick waited on it whenever the game was busy (a shader
 * compile), freezing the page for seconds. An owned window stays above its
 * owner, so the z-order is left alone. */
void place(bool force = false)
{
    static POINT last = { -100000, -100000 };
    RECT rc; GetClientRect(s_game, &rc);
    POINT p = { (rc.right - kW) / 2, (rc.bottom - kH) / 2 };
    if (p.x < 0) p.x = 0;
    if (p.y < 0) p.y = 0;
    ClientToScreen(s_game, &p);
    if (!force && p.x == last.x && p.y == last.y) return;
    last = p;
    SetWindowPos(s_wnd, NULL, p.x, p.y, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
}

bool s_release_wait;     /* closed; the title's input waits for all buttons up */

/* ---- pausing a race (Phase 117) ----
 *
 * Opening the page during a race first presses Start for the title, which
 * pauses it and shows its pause bar. Start toggles, so not when the title is
 * already in a menu: the pause bar and the starting grid's menu are mListBox
 * widgets whose word at +4 (past the vtable) is 3, and whose word at +8 is 7
 * while shown, 3 while hidden (measured with GT5P_MEMSNAP and
 * scripts/diag/memsnap_diff.py; the vtable is menu_listbox_vtable in
 * config/targets.json). They live on the heap, so the page scans guest
 * memory for them when it opens (tens of milliseconds). The heap's range is
 * GT5P_LISTBOX_RANGE=<lo>-<hi> (hex; menu_listbox_range in targets.json),
 * 20000000-40000000 when unset. Gran Turismo 5's objects have the same
 * layout and sit near 0x4F000000 (measured with GT5P_MEMSNAP).
 * GT5P_SETTINGS_PAUSE=0: never press Start. */
enum { kRaceUnknown = -1, kRaceRunning = 0, kRaceInMenu = 1 };
int race_menu_state()
{
    static uint32_t vt = 1;
    if (vt == 1) { const char* e = getenv("GT5P_LISTBOX_VTABLE"); vt = e ? (uint32_t)strtoul(e, nullptr, 16) : 0; }
    if (!vt || !vm_base) return kRaceUnknown;
    static unsigned long long lo = 0x20000000ull, hi = 0x40000000ull; static int range_read;
    if (!range_read) {
        range_read = 1;
        const char* e = getenv("GT5P_LISTBOX_RANGE");
        unsigned long long l = 0, h = 0;
        if (e && sscanf(e, "%llx-%llx", &l, &h) == 2 && l < h && h <= 0x100000000ull) { lo = l; hi = h; }
    }
    int hidden = 0, shown = 0;
    for (unsigned long long a = lo; a < hi;) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(vm_base + a, &mi, sizeof mi)) break;
        const unsigned long long end = (unsigned long long)((uint8_t*)mi.BaseAddress + mi.RegionSize - vm_base);
        if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
            const uint32_t want = _byteswap_ulong(vt);
            const uint32_t* w = (const uint32_t*)(vm_base + a);
            const size_t n = (size_t)((end - a) / 4);
            for (size_t i = 0; i + 2 < n; i++)
                if (w[i] == want && _byteswap_ulong(w[i + 1]) == 3) {
                    const uint32_t st = _byteswap_ulong(w[i + 2]);
                    shown += st == 7; hidden += st == 3;
                }
        }
        a = end;
    }
    if (s_dbg_screens > 0) fprintf(stderr, "[settings-ui] race menus: %d shown, %d hidden%c", shown, hidden, 10);
    return shown ? kRaceInMenu : hidden ? kRaceRunning : kRaceUnknown;
}

ULONGLONG s_inject_until;      /* press Start for the title until then */

void pause_race_if_running()
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("GT5P_SETTINGS_PAUSE"); on = !(e && e[0] == '0'); }
    if (!on || !s_in_race) return;
    if (race_menu_state() != kRaceRunning) return;
    s_inject_until = GetTickCount64() + 150;
    g_pad_inject_buttons = kStart;
    if (s_dbg_screens > 0) fprintf(stderr, "[settings-ui] race running: Start pressed to pause it%c", 10);
}

void set_open(bool open)
{
    if (open == s_open) return;
    s_open = open;
    s_hint_shown = false;   /* the page's image replaces the tag, and closing clears both */
    /* Closing keeps the title's pads at rest until every button is let go
     * (the loop below): the Circle or Cross that closed the page must not
     * reach the game as a new press. */
    if (open) { pause_race_if_running(); g_pad_input_blocked = 1; }
    else s_release_wait = true;
    if (open) {
        s_edit = gt5p_settings_file();
        s_edit.launcher_version=s_boot.launcher_version;
        s_edit.launcher_mod=s_boot.launcher_mod;
        s_sel = 1;          /* the page's first item */
        s_drop = false;
        s_status = gt5p_settings_enabled() ? L"" : L"GT5P_NO_SETTINGS is set: changes are not saved.";
        s_overlay = !legacy_ui_flag() && rsx_d3d12_overlay_available();
        if (s_overlay) { render_overlay(); return; }
        place(true);
        ShowWindow(s_wnd, SW_SHOW);
        SetForegroundWindow(s_wnd);
        SetFocus(s_wnd);
        InvalidateRect(s_wnd, NULL, FALSE);
    } else if (s_overlay) {
        rsx_d3d12_overlay_set(nullptr, 0, 0, 0, 0);
    } else {
        ShowWindow(s_wnd, SW_HIDE);
        SetForegroundWindow(s_game);
    }
}

void set_quit(bool q)
{
    if (q == s_quit) return;
    s_quit = q;
    s_hint_shown = false;
    if (q) { g_pad_input_blocked = 1; s_quit_sel = 0; render_overlay(); }
    else { s_release_wait = true; rsx_d3d12_overlay_set(nullptr, 0, 0, 0, 0); }
}

void quit_choose(int k)
{
    if (k == 1) { set_quit(false); PostMessageW(s_game, WM_CLOSE, 0, 0); }
    else set_quit(false);
}

/* The first button's row (the buttons are last). */
int first_button(const std::vector<int>& r)
{
    int nb = 0;
    for (int id : r) nb += is_button(id);
    return (int)r.size() - nb;
}

/* Up/Down through the rows: the section bar, the items, the buttons as one
 * stop. From a button Down wraps to the bar and Up returns to the last item.
 * With the list open, through its entries. */
void move_sel(int d)
{
    if (s_drop) {
        const std::vector<int> r = rows();
        const int n = (int)items()[r[s_sel]].choices.size();
        s_drop_sel = (s_drop_sel + d + n) % n;
        return;
    }
    const std::vector<int> r = rows();
    const int n = (int)r.size(), fb = first_button(r);
    if (s_sel >= fb) s_sel = d > 0 ? 0 : fb - 1;
    else s_sel = (s_sel + d + n) % n;
    if (s_sel > fb) s_sel = fb;
}

/* L1/R1: the previous or next page. */
void switch_section(int dir)
{
    const int n = (int)sections().size();
    s_section = (s_section + dir + n) % n;
    s_drop = false;
    s_capture = -1;
    s_wcap = -1;
    if (s_sel != 0) s_sel = 1;
}

/* Left/Right: on the bar, switch pages; on an item, step its value; between
 * the buttons, move. Nothing while the list is open. */
void activate(int dir)
{
    if (s_drop) return;
    const std::vector<int> r = rows();
    if (s_sel < 0 || s_sel >= (int)r.size()) return;
    if (r[s_sel] == kRowHead) { switch_section(dir); return; }
    if (r[s_sel] >= 0 && items()[r[s_sel]].key >= 0) {   /* the other column, same line */
        const int other = key_item((items()[r[s_sel]].key + 8) % 16);
        for (int i = 0; i < (int)r.size(); i++) if (r[i] == other) s_sel = i;
        return;
    }
    if (r[s_sel] >= 0 && items()[r[s_sel]].wmap >= 0) {  /* the other column, same line */
        const int other = wmap_item((items()[r[s_sel]].wmap + 10) % 20);
        for (int i = 0; i < (int)r.size(); i++) if (r[i] == other) s_sel = i;
        return;
    }
    if (r[s_sel] >= 0) { change(r[s_sel], dir); return; }
    const int fb = first_button(r), nb = (int)r.size() - fb;
    s_sel = fb + ((s_sel - fb + dir + nb) % nb);
}

/* The highlighted list entry becomes the value. */
void drop_pick()
{
    const std::vector<int> r = rows();
    s_drop = false;
    if (s_sel < 0 || s_sel >= (int)r.size() || r[s_sel] < 0) return;
    const Item& it = items()[r[s_sel]];
    if (s_drop_sel >= 0 && s_drop_sel < (int)it.choices.size() && (s_drop_sel != choice_index(it) || (it.env && !strcmp(it.env,"PAD_MODERN_CONTROLS"))))
        set_value(r[s_sel], it.choices[s_drop_sel].value);
}

/* Cross/Enter: on an item, open its list (or, open, pick); on the bar, the
 * next page; on a button, press it. */
void press()
{
    if (s_drop) { drop_pick(); return; }
    const std::vector<int> r = rows();
    if (s_sel < 0 || s_sel >= (int)r.size()) return;
    if (r[s_sel] == kRowHead) { switch_section(+1); return; }
    if (r[s_sel] >= 0 && items()[r[s_sel]].key >= 0) { begin_capture(r[s_sel]); return; }
    if (r[s_sel] >= 0 && items()[r[s_sel]].wmap >= 0) { begin_wheel_capture(r[s_sel]); return; }
    if (r[s_sel] >= 0) { s_drop = true; s_drop_sel = choice_index(items()[r[s_sel]]); return; }
    change(r[s_sel], +1);
}

/* Circle/Escape: close the list if it is open, else the page. */
void back()
{
    if (s_capture >= 0) { s_capture = -1; s_status = L"Not changed."; }
    else if (s_wcap >= 0) { s_wcap = -1; s_status = L"Not changed."; }
    else if (s_drop) s_drop = false;
    else set_open(false);
}

LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bm = CreateCompatibleBitmap(dc, kW, kH);
        HGDIOBJ o = SelectObject(mem, bm);
        SetGraphicsMode(mem, GM_ADVANCED);
        kKey = RGB(0, 0, 0);   /* the window has no scene behind it */
        paint(mem);
        kKey = RGB(255, 0, 255);
        BitBlt(dc, 0, 0, kW, kH, mem, 0, 0, SRCCOPY);
        SelectObject(mem, o); DeleteObject(bm); DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_KEYDOWN:
        /* Keys posted to this window drive the overlay and the quit question
         * too (tests). */
        if (s_quit) {
            switch (w) {
            case VK_LEFT: case VK_RIGHT: s_quit_sel ^= 1; break;
            case VK_RETURN: case VK_SPACE: quit_choose(s_quit_sel); return 0;
            case VK_ESCAPE: case VK_BACK: set_quit(false); return 0;
            default: return 0;
            }
            redraw();
            return 0;
        }
        if (s_capture >= 0) { capture_key((int)w); redraw(); return 0; }
        if (s_wcap >= 0) {
            if (w == VK_ESCAPE || w == VK_BACK) { s_wcap = -1; s_status = L"Not changed."; redraw(); }
            return 0;
        }
        switch (w) {
        case VK_UP:    move_sel(-1); break;
        case VK_DOWN:  move_sel(+1); break;
        case VK_LEFT:  activate(-1); break;
        case VK_RIGHT: activate(+1); break;
        case VK_RETURN: case VK_SPACE: press(); break;
        case VK_DELETE: {
            const std::vector<int> r = rows();
            if (s_sel >= 0 && s_sel < (int)r.size() && r[s_sel] >= 0 && items()[r[s_sel]].wmap >= 0)
                wheel_clear_slot(r[s_sel]);
            else return 0;
            break;
        }
        case 'I': {
            const std::vector<int> r = rows();
            if (s_sel >= 0 && s_sel < (int)r.size() && r[s_sel] >= 0 && items()[r[s_sel]].wmap >= 1 && items()[r[s_sel]].wmap <= 3)
                wheel_invert_pedal_slot(r[s_sel]);
            else return 0;
            break;
        }
        case VK_PRIOR: switch_section(-1); break;
        case VK_NEXT:  switch_section(+1); break;
        case VK_ESCAPE: case VK_BACK: back(); break;
        case VK_F1: set_open(!s_open); break;
        default: return 0;
        }
        redraw();
        return 0;
    case WM_LBUTTONDOWN:
        click(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_ACTIVATE:
        /* Clicking the game behind the page closes it. */
        if (LOWORD(w) == WA_INACTIVE && s_open && (HWND)l == s_game) set_open(false);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

BOOL CALLBACK find_game(HWND h, LPARAM)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(h) || GetWindow(h, GW_OWNER) != NULL)
        return TRUE;
    wchar_t cls[64] = L"";
    GetClassNameW(h, cls, 64);
    if (!wcscmp(cls, L"ConsoleWindowClass") || !wcscmp(cls, L"PseudoConsoleWindow") ||
        !wcscmp(cls, L"gt5p_settings") || !wcscmp(cls, L"gt5p_profview"))
        return TRUE;
    s_game = h;
    return FALSE;
}

/* The game window's mouse position in layout units, or false when outside. */
bool mouse_units(int* lx, int* ly)
{
    POINT c; GetCursorPos(&c); ScreenToClient(s_game, &c);
    /* The picture's rectangle in the client: all of it, or letterboxed by
     * the backend's output pass (borderless, 1440p). */
    int px = 0, py = 0, pw = 0, ph = 0; rsx_d3d12_output_rect(&px, &py, &pw, &ph);
    int bw = 0, bh = 0; rsx_d3d12_backbuffer_size(&bw, &bh);
    c.x -= px; c.y -= py;
    if (pw <= 0 || ph <= 0 || bw <= 0 ||
        c.x < 0 || c.y < 0 || c.x >= pw || c.y >= ph) return false;
    /* Picture pixels to backbuffer pixels, then to layout units. */
    const double bx = c.x * (double)bw / pw, by = c.y * (double)bh / ph;
    *lx = (int)((bx - s_ox) / s_scale_x); *ly = (int)((by - s_oy) / s_scale);
    return true;
}

DWORD WINAPI ui_thread(LPVOID)
{
    WNDCLASSW wc = {};
    wc.lpfnWndProc = wndproc; wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"gt5p_settings"; wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);
    while (!s_game) { Sleep(500); EnumWindows(find_game, 0); }
    s_wnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW, wc.lpszClassName, L"GT5P settings",
                            WS_POPUP, 0, 0, kW, kH, s_game, NULL, wc.hInstance, NULL);
    SetLayeredWindowAttributes(s_wnd, 0, 242, LWA_ALPHA);
    fprintf(stderr, "[settings-ui] page ready over window %p (%s): F1 or the PS button\n",
            (void*)s_game, s_wnd ? "ok" : "CreateWindow failed");
    s_boot = gt5p_settings_file();
    s_boot.launcher_version=getenv("GT5P_GAME_VERSION") && !strcmp(getenv("GT5P_GAME_VERSION"),"02.11") ? 211 : 104;
    { const char* m = getenv("GT5P_MOD_ACTIVE"); s_boot.launcher_mod = m ? atoi(m) : 0; }
    g_pad_screen_hook = screen_seen;
    rsx_d3d12_on_escape = esc_pressed;
    { const char* e = getenv("GT5P_SETTINGS_UI"); if (e && e[0] == '1') set_open(true); }
    { const char* e = getenv("GT5P_QUIT_UI"); if (e && e[0] == '1') InterlockedExchange(&s_quit_req, 1); }

    int f1_was = 0;
    uint32_t pad_was = 0;
    ULONGLONG repeat_at = 0;
    bool modal_was = false;
    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        if (!IsWindow(s_game)) return 0;

        // Poll results arrive after the UI request; refresh the displayed game value.
        static int modern_was=-2;
        const int modern_now=gt5p_modern_controls_state();
        if(modern_now!=modern_was && (modern_now<0 || !gt5p_modern_controls_pending())) {
            modern_was=modern_now;
            if(modern_now>=0)s_edit.modern_controls=modern_now!=0;
            if(s_open)redraw();
        }
        /* Escape from the game window asks to quit; over the page it closes it. */
        if (InterlockedExchange(&s_quit_req, 0) && !s_open) set_quit(true);

        /* F1 over the game opens the page (inside the legacy window,
         * WM_KEYDOWN closes it; the overlay reads its keys below). */
        const bool game_fg = GetForegroundWindow() == s_game;
        const int f1 = (GetAsyncKeyState(VK_F1) & 0x8000) != 0;
        if (f1 && !f1_was && game_fg && !s_quit) {
            if (!s_open) set_open(true);
            else if (s_overlay) set_open(false);
        }
        f1_was = f1;

        /* Overlay and quit question: the keyboard and mouse while the game
         * window has the focus, a held arrow repeating. A key already held
         * when either opens (the Escape that asked) is not a press. */
        static const int keys[] = { VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_RETURN, VK_SPACE,
                                    VK_ESCAPE, VK_BACK, VK_LBUTTON, VK_PRIOR, VK_NEXT, 'Q', 'W', VK_DELETE, 'I' };
        static uint8_t key_was[256];
        static ULONGLONG key_repeat_at;
        const bool modal = s_quit || (s_open && s_overlay);
        if (modal && !modal_was)
            for (int vk : keys) key_was[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
        modal_was = modal;
        /* Waiting for a binding's key: every key, its first new press. */
        if (s_capture >= 0 && s_open && s_overlay && game_fg) {
            for (int vk = 8; vk < 256; vk++) {
                const bool held = (GetAsyncKeyState(vk) & 0x8000) != 0;
                const bool hit = held && !s_cap_was[vk];
                s_cap_was[vk] = held;
                if (hit && s_capture >= 0) { capture_key(vk); redraw(); }
            }
            if (s_capture < 0)   /* the key that ended the wait is not a page press */
                for (int vk : keys) key_was[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
        } else if (modal && game_fg) {
            const ULONGLONG t = GetTickCount64();
            bool changed = false;
            for (int vk : keys) {
                const bool held = (GetAsyncKeyState(vk) & 0x8000) != 0;
                const bool arrow = vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT;
                bool hit = held && !key_was[vk];
                if (hit && arrow) key_repeat_at = t + 380;
                else if (held && key_was[vk] && arrow && t >= key_repeat_at) { hit = true; key_repeat_at = t + 110; }
                key_was[vk] = held;
                if (!hit) continue;
                if (s_quit) {
                    changed = true;
                    switch (vk) {
                    case VK_LEFT: case VK_RIGHT: s_quit_sel ^= 1; break;
                    case VK_RETURN: case VK_SPACE: quit_choose(s_quit_sel); break;
                    case VK_ESCAPE: case VK_BACK: set_quit(false); break;
                    case VK_LBUTTON: {
                        int lx, ly;
                        if (!mouse_units(&lx, &ly)) break;
                        for (int k = 0; k < 2; k++)
                            if (lx >= kQbtnX[k] && lx < kQbtnX[k] + kBtnW && ly >= kQbtnY && ly < kQbtnY + kBtnH)
                                quit_choose(k);
                        break;
                    }
                    default: changed = false;
                    }
                    continue;
                }
                if (!s_open) continue;
                if (s_wcap >= 0) {
                    if (vk == VK_ESCAPE || vk == VK_BACK) { back(); changed = true; }
                    continue;
                }
                changed = true;
                switch (vk) {
                case VK_UP:    move_sel(-1); break;
                case VK_DOWN:  move_sel(+1); break;
                case VK_LEFT:  activate(-1); break;
                case VK_RIGHT: activate(+1); break;
                case VK_RETURN: case VK_SPACE: press(); break;
                case VK_ESCAPE: case VK_BACK: back(); break;
                case VK_DELETE: {
                    const std::vector<int> r = rows();
                    if (s_sel >= 0 && s_sel < (int)r.size() && r[s_sel] >= 0 && items()[r[s_sel]].wmap >= 0)
                        wheel_clear_slot(r[s_sel]);
                    else changed = false;
                    break;
                }
                case 'I': {
                    const std::vector<int> r = rows();
                    if (s_sel >= 0 && s_sel < (int)r.size() && r[s_sel] >= 0 && items()[r[s_sel]].wmap >= 1 && items()[r[s_sel]].wmap <= 3)
                        wheel_invert_pedal_slot(r[s_sel]);
                    else changed = false;
                    break;
                }
                case VK_PRIOR: case 'Q': switch_section(-1); break;   /* Q/W: the game's L1/R1 keys */
                case VK_NEXT:  case 'W': switch_section(+1); break;
                case VK_LBUTTON: {
                    int lx, ly;
                    if (mouse_units(&lx, &ly)) click(lx, ly);
                    break;
                }
                }
            }
            if (changed) redraw();
        }

        /* Controllers: PS toggles the page; while the page or the question is
         * up, the d-pad and face buttons drive it, a held direction
         * repeating. The buttons come from the game's own pad polls (every
         * frame); polling here as well would run SDL's controller update on
         * two threads at once. */
        const uint32_t pad = g_pad_menu_buttons, down = pad & ~pad_was;
        const ULONGLONG now = GetTickCount64();
        { static int dbg = -1;   /* GT5P_SETTINGS_UI_DBG=1: the buttons the page sees */
          if (dbg < 0) { const char* e = getenv("GT5P_SETTINGS_UI_DBG"); dbg = e && e[0] == '1'; }
          if (dbg && pad != pad_was) fprintf(stderr, "[settings-ui] %llu ms pad %05X\n", now, pad); }
        const uint32_t dirs = kUp | kDown | kLeft | kRight;
        uint32_t act = down;
        if (pad & dirs & pad_was) { if (now >= repeat_at) { act |= pad & dirs; repeat_at = now + 110; } }
        else if (down & dirs) repeat_at = now + 380;
        if (s_quit) {
            if (act & (kLeft | kRight)) { s_quit_sel ^= 1; redraw(); }
            if (act & kCross) quit_choose(s_quit_sel);
            else if (act & kCircle) set_quit(false);
        } else if ((down & kSelect) && !s_open && s_in_options && hint_enabled() && game_fg) set_open(true);
        else if ((down & kPs) && (s_open || game_fg)) set_open(!s_open);
        else if (s_open && (s_capture >= 0 || s_wcap >= 0)) {
            if (act & kCircle) { back(); redraw(); }
        }
        else if (s_open) {
            if (act & kUp) move_sel(-1);
            if (act & kDown) move_sel(+1);
            if (act & kLeft) activate(-1);
            if (act & kRight) activate(+1);
            if (act & kCross) press();
            if (act & kL1) switch_section(-1);
            if (act & kR1) switch_section(+1);
            if (act & kCircle) back();
            if (act && s_open) redraw();
        }
        pad_was = pad;
        if (s_release_wait && !s_open && !s_quit && !(pad & 0xFFFFu)) { g_pad_input_blocked = 0; s_release_wait = false; }
        else if (s_open || s_quit) s_release_wait = false;

        /* The title's Options screen: show the tag while nothing else is up. */
        const bool want_hint = s_in_options && !s_open && !s_quit && hint_enabled() && !legacy_ui_flag() &&
                               rsx_d3d12_overlay_available();
        static int hint_id;
        if (want_hint && s_hint_shown && gt5p_glyphs_select_id() != hint_id) s_hint_shown = false;   /* new device or key */
        if (want_hint != s_hint_shown) {
            if (want_hint) { hint_id = gt5p_glyphs_select_id(); push_image(kHintW, kHintH, paint_hint, true, 0); }
            else if (!s_open && !s_quit) rsx_d3d12_overlay_set(nullptr, 0, 0, 0, 0);
            s_hint_shown = want_hint;
        }

        /* Live wheel capture and telemetry refresh on WHEEL / WHEEL MAP. */
        if (s_open) {
            if (s_wcap >= 0) {
                if (poll_wheel_capture()) redraw();
            }
            if (wheel_page() || wheel_map_page()) {
                static ULONGLONG w_next = 0;
                static WheelRawUi w_last = {};
                if (now >= w_next) {
                    w_next = now + 66;   /* ~15 Hz telemetry refresh */
                    WheelRawUi wr = {};
                    wheel_host_raw(&wr);
                    bool diff = wr.connected != w_last.connected || wr.buttons != w_last.buttons
                             || strcmp(wr.hint, w_last.hint) != 0;
                    if (!diff && wr.connected)
                        for (int a = 0; a < 8; a++)
                            if (fabsf(wr.axis[a] - w_last.axis[a]) >= 0.02f) { diff = true; break; }
                    if (diff) { w_last = wr; redraw(); }
                }
            }
        } else {
            s_wcap = -1;
        }

        if (s_inject_until && GetTickCount64() >= s_inject_until) { g_pad_inject_buttons = 0; s_inject_until = 0; }
        memsnap_poll();
        watch_poll();
        poke_poll();
        toggle_poll();
        game_speed_poll(GetForegroundWindow() == s_game);
        top_menu_poll();
        if (s_open && !s_overlay) place();
        Sleep(16);
    }
}

}  // namespace

/* For the stall dump (GT5P_STALL_MEMSNAP=1, gt5p_present.cpp). */
extern "C" void gt5p_memsnap_write(void) { memsnap_write(); }

/* Start the page unless GT5P_SETTINGS_UI=0. Needs a window, so not headless. */
extern "C" void gt5p_start_settings_ui(void)
{
    const char* e = getenv("GT5P_SETTINGS_UI");
    if ((e && e[0] == '0') || getenv("PS3_HEADLESS")) return;
    HANDLE h = CreateThread(NULL, 256 * 1024, ui_thread, NULL, 0, NULL);
    fprintf(stderr, "[settings-ui] thread %s\n", h ? "started" : "FAILED");
    if (h) CloseHandle(h);
}
