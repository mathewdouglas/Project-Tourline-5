# Plan: NVIDIA DLSS / DLAA (0.2.0), frame generation later

Status (2026-11-24): steps 1–6 are implemented and verified live on 01.04
(`BCUS98114-v104`) on an NVIDIA GeForce RTX 4060:
- Step 1 (`RSX_SCENE_LOG`), step 2 (`RSX_MV=1`), step 3 (`RSX_MV=2`), and step 4
  (`RSX_JITTER=<n>`) are complete and merged on `release/v0.3.0`.
- Step 5 (**DLAA**, `RSX_AA=dlaa` / `RSX_DLAA=1`) is complete: pure-C versioned
  plug-in ABI (`libs/video/rsx_upscale_plugin.h`), standalone NGX/DLSS D3D12
  plug-in (`src/pt5_upscale_dlss.cpp` -> `pt5_upscale_dlss.dll`, optional when
  `third_party/dlss` is present, zero link dependency in `gt5.exe`), single-sample
  scene target when DLAA/DLSS is active, evaluation at the scene/HUD boundary
  draw before HUD composite, automatic fallback after missing DLL / non-RTX /
  3 consecutive eval failures. Live HSR 720p (same non-PGO exe, `RSX_CMD_CHECK=1`):
  DLAA `56.9 FPS` (12.5% late, median `17.0 ms`) vs native 4x MSAA `55.3 FPS`
  (18.4% late, median `17.5 ms`), `0` `RSX_CMD_CHECK` reports, crisp HUD.
- Step 6 (**DLSS Super Resolution**, `RSX_AA=dlss_q|dlss_b|dlss_p|dlss_u|dlss_c`,
  `RSX_DLSS_SCALE`, `RSX_DLSS_PRESET`, `RSX_DLSS_MIP_BIAS`) is complete: renders
  the 3D scene and its own `R24G8_TYPELESS` depth buffer (`s_mv_depth`) and
  motion vectors at the scaled render resolution, scales Halton phases to
  `ceil(8 / scale^2)`, applies `log2(render / output)` `MipLODBias` on filtered
  static samplers (`s8..s15`), upscales to full display resolution before the
  HUD composite, and exposes **NVIDIA DLSS / DLAA** in F1 > VIDEO (`[video] dlss`
  in `gt5p_settings.toml`). Live HSR 1080p Quality (`1280x720 -> 1920x1080`):
  `56.0 FPS` (10.5% late, median `17.0 ms`), `0` `RSX_CMD_CHECK` reports, crisp
  native 1080p HUD.
- Remaining: Step 7 (launcher fetch of `nvngx_dlss.dll`, packaging, VirusTotal)
  and Step 8 (full 01.04/2.11 parity acceptance).

## Goal

1. **0.2.0:** DLAA (anti-aliasing at native size) and DLSS Super Resolution
   (Quality / Balanced / Performance / Ultra Performance, plus a custom
   scale) for the 3D scene, on both engines (01.04 and 2.11), D3D12 only.
   Off by default; every new behaviour has an env switch.
2. **Later:** frame generation, fed by the same motion vectors and depth.

The HUD and menus are never upscaled: DLSS runs on the 3D scene only, before
GT5 draws its HUD over it.

## What DLSS needs from us

| Input | What it is | Where it comes from (plan) |
| --- | --- | --- |
| Colour | The 3D scene before HUD, at render size, not multisampled | GT5's own scene target, located by the scene/HUD boundary (step 1) |
| Depth | That scene's depth buffer | The zeta bound while the scene was drawn |
| Motion vectors | Per pixel, where it was last frame, in render-size pixels | Built by us (steps 2-3): GT5 does not keep a velocity buffer |
| Jitter | A sub-pixel camera offset per frame, the same one DLSS is told | Added by us to the viewport transform of scene draws (step 4) |
| Reset | History is invalid (cut, new scene, size change) | Our own scene tracking |
| Exposure | Optional | DLSS auto-exposure; GT5's own exposure runs after DLSS anyway |

## What the game gives us (research so far)

### GT5's own motion blur: camera only

Static analysis (no live run) found GT5's scene motion blur in both
executables:

| | 01.04 | 2.11 |
| --- | --- | --- |
| Motion-blur renderer routine | `0x00A06844` | `0x008AC9D0` |
| History-reset routine | `0x00A06834` | `0x008AC9C0` |
| Fragment program (`SceneMotionBlur`) structural hash | `3FD75959` | `1BBA1ADE` |

The renderer type `PDIScene::SceneMotionBlurRenderer` takes `depthMap`,
`colorMap`, `baseMap`, `pmat`, `shutterRate` and `ithreshold`. The shader reads
depth once and colour eight times along a step it computes from the pixel's
position, its depth and a 4x4 matrix (`pmat`). The CPU side keeps the previous
matrices, subtracts, scales by the shutter and resets history on large
camera jumps. That is depth-based camera reprojection: it has no per-object
motion, and three archived 01.04 race manifests contain no draw with this
program, so when it runs is unknown. It is a fallback source for camera
motion, not the plan.

### Per-object motion from the vertex constants

Another GT5 port (MormonFTP-hub/gt5-recomp, MIT licence, commit `9030daf`,
2026-11-17) shipped DLSS/DLAA. Its renderer is a different D3D12 path in its
own ps3recomp fork and targets the European disc build, so its code does not
drop into ours; its design does, and it needs no guest addresses. Their
measurements, not ours:

- **Every GT5 vertex program builds clip position from `c[0..3]`** (object to
  clip). One object keeps one `c[0..3]` for all its draws in a frame.
- **`c[4..6]` is object to world**, so `c[0..3] * inverse(c[4..6])` is the
  frame's view-projection, the same for every object of one viewport (to
  1e-7 on track, player car, wheels and other cars). Mirrors have their own
  viewport and camera.
- **Objects are matched to last frame's** by their transform (position and
  rotation distance, same vertex program and vertex count as a tie-break,
  with a gate beyond which there is no match).
- **Matched draws write their own vector** into a second render target: the
  vertex shader also outputs last frame's position (rows of
  `pixel * viewport_prev * c_prev * inverse(c_now)`), the fragment shader the
  difference. A full-screen pass over depth fills every other pixel from the
  camera alone.
- **The scene/HUD boundary** is the first draw of a frame that writes colour
  into a display buffer with depth test and blending off and samples a
  surface drawn with depth testing earlier that frame, with that zeta still
  bound. That surface is the scene, that zeta its depth. Menus, movies and
  Photo Mode fail the test and are left alone.
- **DLSS output replaces the scene in that one draw's texture slot,** so
  GT5's exposure, bloom and colour work run on the upscaled picture and the
  HUD is drawn over it as usual.

If we reuse any of their code, their MIT notice goes into `licenses/`.

## What we already have

- Render scaling: `RSX_RENDER_HEIGHT`, `uw_host_w`/`uw_host_h`, `rs_on()`,
  race full width (`RSX_RACE_FULLWIDTH`, `OffRT.gw`) in
  `libs/video/rsx_d3d12_backend.c`.
- The output pass (`RSX_OUTPUT_PASS`, `RSX_OUTPUT_SIZE`).
- 16-bit colour targets (`RSX_HIGH_PRECISION`), which DLSS can take as LDR.
- Readbacks in command order and per-draw descriptor tables (texture slot
  substitution is one SRV write).

## What has to change

- **MSAA:** `RSX_MSAA_FORCE` multisamples the window-sized scene target. With
  DLSS on, the scene must be single-sampled (DLSS is the anti-aliasing).
- **Pipeline keys and the learned lookup table:** the motion-vector target
  and the jittered/LOD-biased variants change pipelines. Every new switch
  that changes translation must be part of the key and of `fplk_epoch`, or
  `RSX_PSO_FAST_LOOKUP` serves wrong pipelines. Race packs and recipes
  learned without DLSS must not be served to it.
- **Texture LOD bias:** `log2(render / output)` on scene samplers while the
  scene is drawn below output size. The sampler bank is static samplers in
  the root signature, so the bias needs a second bank (a root-signature
  change rebuilds pipelines once) or a shader-side bias.
- **Sizes:** check ultrawide (`uw_host_w`, pillarbox `vp_posoffset.w`), 1440p
  and 4K, and the 1080p mode's 2304-wide clip inside a wider surface.
- **Ordering:** the evaluation runs on the replay thread's command list at the
  boundary draw; NGX is single-threaded.

## Shipping NVIDIA's part

NVIDIA's SDK (headers, `nvsdk_ngx` library, `nvngx_dlss.dll`) is not ours
and is not committed. Options:

1. **Ship our plug-in prebuilt** (`pt5_upscale_dlss.dll`, our source linked
   with NVIDIA's static library) and have the launcher fetch
   `nvngx_dlss.dll` from NVIDIA's public repository at a pinned release,
   checking SHA-256 and the NVIDIA Authenticode signature, as it does for
   RPCS3.
2. **Ship both DLLs in the ZIP**, if NVIDIA's licence allows redistribution
   with the notices it asks for.

The other port builds the plug-in on the player's PC; our players have no
compiler, so that does not fit. **NVIDIA DLSS SDK licence review (2026-11-24,
`NVIDIA/DLSS` `v310.9.16` `LICENSE.txt`):**
- §1(c) permits distributing SDK materials only as incorporated in object code
  format into a software application that provides material additional
  functionality (§2(a)), subject to terms at least as protective (§2(c)) and
  attribution (§7.1(b)).
- §4(e) prohibits causing the SDK to become subject to an open-source licence,
  so SDK headers, `nvsdk_ngx_*.lib`, and `nvngx_dlss.dll` must **never** be
  committed to the git repository (kept in gitignored `third_party/dlss`).
- Supplement §4 notification applies only to *commercial* releases.
- **Recommended path (Option 1):** ship our small prebuilt `pt5_upscale_dlss.dll`
  (~54 KB, linked with `nvsdk_ngx_d.lib`, accompanied by
  `NVIDIA-DLSS-LICENSE.txt` notice) and have the launcher fetch
  `nvngx_dlss.dll` (~59 MB) from the pinned `NVIDIA/DLSS` GitHub release tag on
  demand, verifying SHA-256 and NVIDIA Authenticode signature (or allow the user
  to drop `nvngx_dlss.dll` beside the executable). This avoids bloating the base
  release ZIP by ~59 MB and avoids hosting NVIDIA's standalone runtime DLL on
  our GitHub Releases. Whatever ships goes through the VirusTotal check
  (`docs/release-av.md`). The game runs unchanged with no plug-in: it is loaded
  at run time and falls back cleanly when missing, non-RTX, or driver too old.

## Steps

Each step ends with a measurement on both engines before the next starts.

1. **Scene/HUD boundary, logging only** (`RSX_SCENE_LOG=1`). A `[scene]` line
   when the judgement changes (scene / held / bypass, with the reason). Check
   at High Speed Ring, Eiger rain, cockpit view, replay, car list, menus,
   Photo Mode, 21:9. Nothing changes in the picture.
2. **Camera-only motion vectors** (`RSX_MV=1`) from depth and the recovered
   view-projection, with a debug view (`RSX_MV_VIEW`, hue = direction) and a
   dump of two consecutive frames. Check: reprojecting frame N-1 by the
   vectors matches frame N on static scenery.
3. **Per-object vectors** (`RSX_MV=2`): object table, matching, second render
   target, fill pass. Check on other cars, wheels, the player's car in chase
   and cockpit views, mirrors.
4. **Jitter** (`RSX_JITTER=<n>`, Halton(2,3)). Check with a
   multi-offset capture that the offsets land where stated, sign included.
   Done (2026-11-20).
5. **DLAA** (`RSX_AA=dlaa`): plug-in interface, load, evaluate at the
   boundary, substitute the slot, reset handling, failure fallback (after N
   failures switch off with the reason in F1). GPU time on the F3 page.
6. **DLSS modes** (`dlss_q|b|p|u|c`, `RSX_DLSS_SCALE`, `RSX_DLSS_PRESET`):
   texture LOD bias `log2(render / output)` on scene samplers (moved here
   from step 4; DLAA needs none),
   scene drawn at the render size through the existing scaling, restored
   when no scene is seen. F1 VIDEO row, launcher setting, settings file key.
7. **Shipping:** licence decision, launcher fetch or packaging, tests in
   `scripts/test/player_setup.py`, VirusTotal.
8. **Acceptance:** `race.ps1 -FrameTest` with each mode against native, image
   checks on fences, wheels and mirrors, 2.11 parity.

## Frame generation (after 0.2.0)

The motion vectors and depth from steps 2-3 are what any generator needs.
Options: our own generator using them (the other port's approach; it never
touches the HUD because it works on the scene before the HUD and redraws
nothing else), or NVIDIA DLSS Frame Generation through Streamline, which
also needs Reflex and a swap-chain proxy and is a much larger change.
Decide after 0.2.0.

## Open questions

- Whether `c[0..3]` / `c[4..6]` hold on our two executables exactly as
  measured on the European disc build (step 2 measures it).
- Whether 2.11 draws anything into the scene differently (cockpit windows,
  new cars).
- NVIDIA's redistribution terms.
- DLSS model preset default.
