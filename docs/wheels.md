# Steering wheels

Status (2026-11-23, branch `release/v0.2.0`): built into `gt5.exe` and **verified
on real hardware (Logitech G29, `046D:C24F`)** on GT5 01.04 — USB attach,
steering, pedals (clutch on `S0`), buttons, live force feedback, `F1 > WHEEL`
presets/FFB tuning, and `F1 > WHEEL MAP` interactive binding all work. Note: on
Windows, Logitech G29/G923 require **Logitech G HUB** (not Logitech Gaming
Software) to switch out of PS4 mode (`046D:C260`) and serve DirectInput FFB;
`F1 > WHEEL` now detects both conditions and shows an explicit hint.

## How GT5 uses a wheel

On a PS3, GT5 drives Logitech wheels itself. It registers its own USB driver
with `cellUsbd`, reads the wheel's 11-byte HID reports, sends Logitech's
force-feedback commands back, and shows the wheel to the rest of the game as a
custom controller through `cellPadLdd*`.

GT5 01.04 (BCUS98114) has this driver. The vendor and product check is in
`func_00C5E154` and the functions near it. It accepts Driving Force,
Driving Force Pro, G25, Driving Force GT, G27 and MOMO
(`046D:C294`, `C298`-`C29B`, `CA03`). The 2.11 executable has not been checked.

Before this branch the runtime's `cellUsbd` was a stub and `cellPadLdd*` did not
exist; 9 of GT5's 13 `cellUsbd` imports and 2 of its pad imports were
unregistered.

## What the port does

The runtime shows the game one **emulated Logitech G27** (`046D:C29B`), whatever
wheel is on the PC, so GT5's own driver does the rest:

| Part | File (runtime patch) | Job |
| --- | --- | --- |
| USB bus | `libs/hardware/cellUsbd.c` | Calls the game driver's probe and attach, answers control transfers, takes force-feedback writes, completes report reads, on a worker thread |
| The G27 | `libs/hardware/wheel_g27.c` | Descriptors, the input report, the force-feedback command decoder (4 slots and the default spring) |
| The PC wheel | `libs/hardware/wheel_host_win.c` | DirectInput: reads the wheel every millisecond and plays the decoded effects on it |
| Custom pad | `libs/input/cellPad.c` | `cellPadLdd*`: the port the game's driver feeds |

The USB layer and the G27 are adapted from
[gt5-recomp](https://github.com/MormonFTP-hub/gt5-recomp) (MIT,
`third_party_notices/gt5-recomp-LICENSE.txt`), which targets the EU build
(BCES00569). Their files work at the `cellUsbd` level and use no game
addresses, so no per-build constants are needed here. Changes from theirs:

- Their mouse steering and in-game menu hooks are left out.
- The descriptor page comes from the runtime's scratch allocator, not a fixed
  page of the HLE window.
- The settings page's input block (`g_pad_input_blocked`) gives the game a
  centred rim and released pedals, as it does for pads.
- Forces pause while F1 is open or the game window is in the background.
- Spring/damper scaling, a minimum steering force, and live tuning
  (`wheel_host_set_ffb`).
- SDL leaves wheels alone (`pad_sdl_is_wheel`), so a wheel SDL knows as a
  game controller does not also drive port 1.

## Settings

F1 > **WHEEL** (and **WHEEL MAP** for interactive axis/button binding), or `[wheel]` in `gt5p_settings.toml`. Presets, axis/button bindings, and force-feedback rows apply live; the others after a restart.

| Setting | Key | Variable | Default |
| --- | --- | --- | --- |
| Steering wheel | `enabled` | `PS3_WHEEL` (`auto`, `off`, `virtual`, or part of a name / `vid:pid`) | On |
| Wheel preset | `preset` | — (`auto`, `g29`, `g920`, `g27`, `thrustmaster`, `custom`) | Auto (by model) |
| Wheel rotation | `rotation` | `PS3_WHEEL_RANGE` | 900 |
| Force feedback strength | `ffb_strength` | `PS3_WHEEL_FFB` (0-100) | 80 |
| Force direction | `ffb_invert` | `PS3_WHEEL_FFB_INVERT` | Normal |
| Centring and damping | `ffb_springs` | `PS3_WHEEL_FFB_SPRING` (0-200 %) | 100 |
| Minimum force | `ffb_min_force` | `PS3_WHEEL_FFB_MIN` (0-50 %) | 0 |
| Pause forces in background | `ffb_pause` | `PS3_WHEEL_FFB_MUTE` | On |
| Steer axis | `steer` | `PS3_WHEEL_STEER` | Auto (`X`) |
| Pedal axes | `pedals` | `PS3_WHEEL_PEDALS` (e.g. `throttle=Y,brake=RZ,clutch=Z`, append `inv` to invert) | Auto |
| Wheel buttons | `buttons` | `PS3_WHEEL_BUTTONS` (e.g. `cross=1,circle=3,start=10`) | Auto |

On **F1 > WHEEL MAP**, press Enter/Click on any axis (`Steer`, `Throttle`, `Brake`, `Clutch`) or button (`Shift Up`, `Shift Down`, `Cross`–`Select`) and turn the wheel, press the pedal, or press the wheel button to bind it live. Press **I** on a pedal row to toggle axis inversion, **Delete** to clear a binding, or **Defaults** to return to `Auto`. Live connected-wheel telemetry (device name, VID:PID, FFB status, axes, and pressed button number) is shown at the bottom of both **WHEEL** and **WHEEL MAP**.

Variables with no settings row:

- `PS3_WHEEL_SHIFTER` and `PS3_WHEEL_MENU`.
- `PS3_WHEEL_TRACE=1` logs axes, reports and force commands.
- `PAD_SDL_WHEELS=1` lets SDL read wheels as pads again.

Defaults are laid out for the G29/G923 PlayStation models (throttle Y, brake RZ,
clutch Z, G29 button order). Thrustmaster's separate-pedal layout is built in;
other wheels may need `PS3_WHEEL_PEDALS`/`_BUTTONS`.

## Checks

- `scripts/test/wheel_g27.py`: the descriptors, report bytes, rotation scaling,
  force decoding and menu detection, on the runtime's real `wheel_g27.c`.
  It passes.
- All 16 USB/pad imports GT5 01.04 makes resolve to these functions
  (`gen_hle_nids.py --all`; `src/gen/ppu_hle_nids.cpp`).
- Built into `build/gt5.exe` (`release/v0.2.0`) and verified with a scripted
  `PS3_WHEEL=virtual` High Speed Ring race (`scripts/test/gt5boot.ps1 -Tag wheel_virt -Route race -Settle 15 PS3_WHEEL=virtual PS3_WHEEL_TRACE=1`):
  `[cellUsbd] Logitech G27 (emulated) attached to the game's driver`,
  `[cellPad] LddRegisterController -> port 4`, ~112 reports/s, live FFB
  `dl+play`/`stop` and shift-LED commands received cleanly at 60 FPS.
- **Live hardware check passed (Logitech G29, `046D:C24F`, GT5 01.04):**
  - Attached cleanly as `[cellUsbd] Logitech G27 (emulated) attached to the game's driver` (`046D:C24F`, 900°, 25 buttons, POV d-pad, axes `X Y RZ S0`).
  - Steering, pedals, buttons, and full force feedback (with Logitech G HUB running) verified in-game.
  - Caught & fixed: G29 clutch axis is `S0` (not `Z`); added automatic detection + F1 UI hints for G29/G923 stuck in PS4 mode (`046D:C260`/`C266`) and DirectInput FFB `E_NOTIMPL` when G HUB is not running.

## Open

- An Xbox-mode wheel (G920/G923 Xbox) also appears to XInput; with SDL2 absent
  the XInput path would read it as a pad. Not handled.
- 2.11's wheel driver is not checked.
- The RecompLauncher manifest has no wheel options yet.
