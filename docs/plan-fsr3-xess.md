# Plan: AMD FSR 3.1 / 4 and Intel XeSS beside DLSS (research, 2026-11-26)

Status: research only. No code changes. Based on `origin/main` `b1222e1`
(release 0.3.1). Read [plan-dlss.md](plan-dlss.md) and the 0.3.0 and 0.3.1
entries of [progress-gt5.md](progress-gt5.md) first.

Labels used below. **Measured** means a value from our code or from our own
earlier runs. **Vendor doc** means read on 2026-11-26 from the vendor's
current documentation. **Hypothesis** means not checked yet. A hypothesis
must be measured before code depends on it.

## 1. Summary

- **FSR Super Resolution and Native AA fit our existing path with almost no
  backend work.** FSR takes the same inputs DLSS takes now: the scene before
  the HUD, depth, motion vectors in render pixels from current to previous,
  and pixel jitter. The work is a new plug-in DLL plus generalising the
  plug-in ABI. FSR 3.1 runs on every DX12 GPU with Shader Model 6.2,
  including our RTX 4060, so we can test it locally. FSR 4 (the ML upscaler)
  arrives through the same API on RX 9000 GPUs only, and we have none to test
  on.
- **XeSS Super Resolution and Native AA are also a plug-in-only change.** XeSS
  uses DP4a on Shader Model 6.4 on NVIDIA and AMD GPUs, and XMX on Intel Arc.
  Its quality ratios differ from DLSS and FSR, so the backend's hard-coded
  scale table (`dlss_scale_factor()`) has to come from the plug-in instead.
- **FSR Frame Generation is the large item.** FSR 3.1 replaces the swap chain
  with a proxy (`ffxCreateContextDescFrameGenerationSwapChainForHwndDX12` or
  `...WrapDX12`). The proxy owns pacing and presenting. Our DLSS-G path is the
  opposite: we call NGX for the generated frames and present them ourselves
  (`fg_present_extra`). The ABI therefore needs a second frame-generation
  mode, "plug-in owns the swap chain", and the backend must create its swap
  chain through the plug-in at startup.
- **Separate upscaler and frame generator are already possible in our
  design.** Super resolution runs at the scene/HUD boundary and frame
  generation runs at present, sharing `s_mv_tex` and depth. FSR 3.1 separated
  its frame generation from its upscaler for this purpose. Pairings such as
  DLSS SR with FSR FG on an RTX 30 card need no new inputs. The one rule: only
  one frame generator may own the swap chain, so changing the frame-generation
  vendor needs a restart.
- **XeSS-FG: defer it.** It requires XeLL (Intel's latency reduction) with
  per-frame simulation, render and present markers plus a sleep call before
  input sampling. A recompiled guest has no clean place for any of these. It
  also takes over the swap chain the same way FSR does, and Intel's own guide
  contradicts itself about non-Intel support (Arc with XMX in the guide,
  SM 6.4 in the README). Revisit after FSR FG ships.
- **Licensing corrects the brief.** The current AMD SDK ("FSR SDK 2.4.0
  Redstone") is not plain MIT. Its top-level `docs/license.md` is an AMD grant
  for redistribution in binary form only, with no reverse engineering and the
  notices reproduced. The samples, and the API headers I read, carry MIT
  notices. The old FidelityFX SDK 1.1.x (FSR 3.1.x) was MIT with full source
  (hypothesis: re-check the tag's LICENSE before relying on it). XeSS uses the
  Intel Simplified Software License: binaries may be redistributed unmodified
  with the notice; no reverse engineering. Nothing vendor-made is ever
  committed, exactly as for DLSS.

## 2. What we have now (0.3.1, measured from the code)

| Piece | Where | Detail |
| --- | --- | --- |
| ABI | `libs/video/rsx_upscale_plugin.h` | `PT5_UPSCALE_ABI_VERSION 1`; exported functions `pt5_upscale_{abi_version,init,optimal,create,eval,destroy_feature,shutdown}`, `pt5_fg_{caps,create,eval,destroy}`; one global feature per DLL |
| Plug-in loading | `dlss_ensure_init()` | Loads `pt5_upscale_dlss.dll` by fixed name; refuses any ABI other than exactly 1 |
| Mode / scale | `dlss_want_mode()`, `dlss_scale_factor()` | `RSX_AA=dlaa|dlss_q|b|p|u|c`; scales 0.667 / 0.58 / 0.50 / 0.333 hard-coded in the backend |
| Colour | `dlss_eval_frame()` | `off_rt[scene_rt].res`; scene RT is `R16G16B16A16_UNORM` (the title's 8-bit target promoted, `RSX_HIGH_PRECISION`); `is_hdr=1` for that format, override `RSX_DLSS_HDR` |
| Depth | same | `s_mv_depth_cur` (`R24G8_TYPELESS` copy, own depth for scaled targets) or `s_depth_copy`; standard Z (0 near, 1 far), `depth_inverted=0` |
| Motion vectors | `s_mv_tex` | `R16G16_FLOAT`, `prev_px - curr_px` in render pixels, per-object plus camera fill, jitter removed on the CPU (`RSX_MV_OBJ_UNJIT`); `mv_scale = 1` for SR, `1/size` for DLSS-G |
| Jitter | `FR->mv.jit` | Halton(2,3), `ceil(8/scale^2)` phases; DLSS gets `+1 * jit` in render pixels (`RSX_DLSS_JITTER_SIGN`, sign measured by 8-frame static MAE) |
| Mip bias | sampler bank s8..s15 | `log2(render/output)` |
| SR output | `s_dlss.out_res` | UAV at display size, substituted into unit 0 of the boundary draw `d == FR->mv.disp` |
| FG inputs | `fg_eval_frame()` | backbuffer (`R8G8B8A8_UNORM`), depth converted to `R32_FLOAT` (`fg_depth_convert`), `s_mv_tex`, hudless copy from just after the boundary draw (`fg_hudless_capture`), camera from `FR->mv.vp` (`fg_extract_camera`), `clip_to_prev` from `FR->mv.reg[0].R` |
| FG present | `fg_present_extra()` | Generated frame copied into the backbuffer and presented with sync 0; further generated frames and the real frame (kept in `s_fg.real_copy`) presented by `fg_present_resource()` on a second command list; the real frame uses `s_psync`; `RSX_FG_PACE=1` adds an opt-in sleep; no Reflex |
| Swap chain | runtime init, about line 2117 | `CreateSwapChainForHwnd` on the direct queue, `FLIP_DISCARD`, `FRAME_COUNT 3` (0.3.1), `R8G8B8A8_UNORM`, `DXGI_MWA_NO_ALT_ENTER`; no `ResizeBuffers` anywhere (a size change is a restart); borderless is a window style (`RSX_WINDOW_MODE`), never exclusive fullscreen |
| F1 overlay | `rsx_d3d12_overlay_set` | Blended into the backbuffer before `fg_eval_frame`, so it is part of the FG colour input and missing from hudless |
| State restore | after each external eval | `SetDescriptorHeaps`, root signature, `D3D_SD_RESET()` |

All of this except the vendor calls inside the DLL is vendor-neutral. That is
why the cost of FSR and XeSS SR is mostly the plug-in.

## 3. Feasibility and SDK requirements

| | DLSS (have) | FSR 3.1.x | FSR 4 (ML) | XeSS SR 2.x/3 | XeSS-FG | FSR FG 3.1.x | FSR FG 4.0 (ML) |
| --- | --- | --- | --- | --- | --- | --- | --- |
| GPU | RTX | any DX12, SM 6.2, typed UAV load, `R16G16B16A16_UNORM` UAV | RX 9000 (RDNA 4) | Arc/Xe (XMX), others SM 6.4 + DP4a | Arc with XMX per guide; SM 6.4 non-Intel per README (conflict) | any DX12 GPU | RX 9000 |
| Driver | NGX version check | none special | Adrenalin; automatic through the 2.x loader | Intel driver for XMX | Intel 32.0.101.6252+ (guide) | HAGS recommended | Adrenalin 26.10.1+ for driver-assisted pacing |
| Queues | ours | ours (app command list) | same | ours | its proxy submits on the queue given at init and presents on its own thread | proxy: present queue or async compute (`FFX_FRAMEGENERATION_ENABLE_ASYNC_WORKLOAD_SUPPORT` + `allowAsyncWorkloads`), a second graphics queue for UI composition, 2 extra CPU threads, one busy-waits | same |
| Input states | `NON_PIXEL_SHADER_RESOURCE`, output UAV | same | same | same (XeSS does no synchronisation itself) | `NON_PIXEL_SHADER_RESOURCE` (valid until present) or `COPY_SOURCE` (copied now) | prepare inputs `NON_PIXEL_SHADER_RESOURCE` | same |
| Minimum input FPS | none stated | n/a | n/a | n/a | n/a | 3.1.6: 60+; 3.1.7: 30+ with weaker pacing | 30+ |

API entry points (vendor doc):

- **FSR SR**: `ffxCreateContext` with `ffxCreateContextDescUpscale` (flags
  `FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE`, `_DEPTH_INVERTED`,
  `_DEPTH_INFINITE`, `_AUTO_EXPOSURE`, `_MOTION_VECTORS_JITTER_CANCELLATION`,
  `_DISPLAY_RESOLUTION_MOTION_VECTORS`, `_NON_LINEAR_COLORSPACE`), chained
  `ffxCreateContextDescUpscaleVersion`; per frame `ffxDispatchDescUpscale`
  (`color`, `depth`, `motionVectors`, `exposure`, `reactive`,
  `transparencyAndComposition`, `output`, `jitterOffset`,
  `motionVectorScale`, `renderSize`, `upscaleSize`, `enableSharpening`,
  `sharpness`, `frameTimeDelta` ms, `preExposure` > 0, `reset`,
  `cameraNear/Far`, `cameraFovAngleVertical`). Queries:
  `ffxQueryDescUpscaleGetJitterPhaseCount`, `...GetJitterOffset`.
  Ratios: Native AA 1.0, Quality 1.5, Balanced 1.7, Performance 2.0,
  Ultra Performance 3.0.
- **XeSS SR**: `xessD3D12CreateContext`, `xessD3D12BuildPipelines`
  (non-blocking is available; use it at startup), `xessD3D12Init` (output
  size, quality, flags fixed after init), `xessD3D12Execute` (jitter in
  [-0.5, 0.5], `exposureScale` or `pExposureScaleTexture`,
  `resetHistory`, responsive pixel mask), `xessGetOptimalInputResolution`,
  `xessSetJitterScale`, flags `XESS_INIT_FLAG_LDR_INPUT_COLOR`,
  `_INVERTED_DEPTH`, `_HIGH_RES_MV`, `_USE_NDC_VELOCITY`,
  `_EXTERNAL_DESCRIPTOR_HEAP`, auto exposure. Since XeSS 1.3 the ratios are
  Native AA 1.0, Ultra Quality Plus 1.3, Ultra Quality 1.5, Quality 1.7,
  Balanced 2.0, Performance 2.3, Ultra Performance 3.0 (from memory, not
  re-read; take them from `xessGetOptimalInputResolution` at run time, never
  from a table).
- **FSR FG**: swap chain created by `ffxCreateContext` with one of
  `ffxCreateContextDescFrameGenerationSwapChainForHwndDX12`
  (`hwnd`, `desc`, `fullscreenDesc`, `dxgiFactory`),
  `...NewDX12` or `...WrapDX12`, each chained with
  `ffxCreateContextDescFrameGenerationSwapChainVersionDX12`; the result is an
  `IDXGISwapChain4`. A frame-generation context
  (`ffxCreateContextDescFrameGeneration`, same depth/MV/HDR flags plus
  `ASYNC_WORKLOAD_SUPPORT`); each frame `ffxConfigureDescFrameGeneration`
  (`swapChain`, `frameGenerationEnabled`, `allowAsyncWorkloads`,
  `HUDLessColor`, `presentCallback`, `frameGenerationCallback`,
  `generationRect`, `frameID`, `flags`) and
  `ffxDispatchDescFrameGenerationPrepareV2` (`frameID`, `renderSize`,
  `jitterOffset`, `motionVectorScale`, `frameTimeDelta`, `reset`,
  `cameraNear/Far`, `cameraFovAngleVertical`, `viewSpaceToMetersFactor`,
  camera position/up/right/forward, `depth`, `motionVectors`). The older
  `...Prepare` and `...PrepareCameraInfo` are deprecated. The header also has
  `ffxDispatchDescFrameGeneration` (`presentColor`, `outputs[4]`,
  `numGeneratedFrames`, `generationRect`, `frameID`): that is the dispatch
  the swap chain's callback runs. Whether an application may call it alone,
  without the proxy, and present the outputs itself (our DLSS-G model) is a
  **hypothesis to prototype**. If it works it is the cheapest FSR FG path.
- **XeSS-FG**: `xefgSwapChainD3D12CreateContext`,
  `xefgSwapChainSetLatencyReduction(ctx, xellCtx)`, then
  `xefgSwapChainD3D12InitFromSwapChain(ctx, queue, &params)`. This takes our
  swap chain, which must have a reference count of exactly 1; it fails in
  exclusive fullscreen. Alternatively `...InitFromSwapChainDesc`. Then
  `xefgSwapChainD3D12GetSwapChainPtr`, which every later swap-chain call must
  go through. Per frame: `xefgSwapChainD3D12TagFrameResource` (MV, depth,
  hudless, UI), `xefgSwapChainTagFrameConstants` (row-major view and
  projection without jitter, jitter, MV scale, `resetHistory`, GPU frame
  time), `xefgSwapChainSetPresentId`, then Present. On or off:
  `xefgSwapChainSetEnabled`. UI modes `XEFG_SWAPCHAIN_UI_MODE_*`
  (`BACKBUFFER_HUDLESS` = hudless only, UI extracted). HDR only as
  `R10G10B10A2` PQ. XeLL: `xellD3D12CreateContext`, `xellSetSleepMode`, and
  every frame `xellSleep(frameId)` before input, plus all six
  `xellAddMarkerData` markers (`SIMULATION_START/END`,
  `RENDERSUBMIT_START/END`, `PRESENT_START/END`). With XeLL on, Present must
  use sync interval 1. Not compatible with other frame generators.

## 4. Input and convention deltas

### 4.1 Colour space

Measured: the scene target is `R16G16B16A16_UNORM`, so values are clamped to
[0,1]; GT5's exposure, bloom and tone map run after our evaluation. The brief
calls it "pre-tonemap linear HDR". The format cannot hold values above 1, and
whether GT5 stores linear or gamma-encoded values there is **not measured**.
The guest has an sRGB-on-write control (method `0x1FEC`; capture manifests
print `gam=` and `sw=` per draw).

- DLSS accepted `is_hdr=1` on this and looks right, so it says little.
- FSR: `FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE` requires linear input. For a
  [0,1] perceptual (gamma) signal, the LDR path with
  `_NON_LINEAR_COLORSPACE` (or `FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB` per
  dispatch) is correct. For a linear [0,1] signal use LDR without that flag.
  HDR mode on [0,1] data is legal but wastes precision in FSR's internal tone
  map.
- XeSS: `XESS_INIT_FLAG_LDR_INPUT_COLOR` for [0,1] input.
- **First measurement of Phase 1:** read `gam=`/`sw=` of the scene draws in an
  `RSX_CAP` race capture, and check one known texel's value against the
  displayed pixel. Expose the result as one backend value
  (`Pt5ColorSpace`: `LINEAR_HDR`, `LINEAR_LDR`, `SRGB_LDR`; override
  `RSX_UPSCALE_COLOR`) passed to every plug-in, not as a per-vendor guess.

### 4.2 Exposure

GT5 meters exposure after our evaluation (the 16x8 blit of Phase 118), so the
upscaler input is not pre-exposed. Use `preExposure = 1.0`, no exposure
texture, and no auto exposure for FSR (it then uses 1.0). For XeSS use
`exposureScale = 1.0`, auto exposure off. DLSS currently has its own
`auto_exp` setting; keep DLSS unchanged and measure if FSR or XeSS show
pumping in tunnels (High Speed Ring tunnel is the test).

### 4.3 Reactive, transparency/composition and responsive masks

Because we evaluate before the HUD, the main FSR ghosting source (HUD and
text over a moving scene) never reaches the upscaler. What is left is
alpha-blended or animated content inside the scene, where FSR without a mask
smears or loses detail. DLSS copes without masks; XeSS sits between the two.
Candidates in GT5:

| Content | Risk without mask | Note |
| --- | --- | --- |
| Rain drops and spray (Eiger, rain weather) | High: streaks and ghost trails | Particles, blended, no motion vectors of their own |
| Tyre smoke, dust, sparks | High | Same |
| Car glass and headlamp glass | Medium: reflections ghost | Blended over the interior |
| Rear-view mirror | Medium | Its own viewport and camera; MVs "open" per plan-dlss |
| Alpha-to-coverage foliage, crowd sprites | Medium: shimmer | A2C substitute is an alpha test at 0.5 |
| Animated screens, water | Low to medium | |

How to make a mask, cheapest first (all hypotheses until measured):

1. **None** (NULL; FSR uses an internal cleared texture). Ship Phase 1 this
   way and look.
2. **FSR's generator from an opaque-only snapshot.** FSR has a helper
   dispatch that builds a reactive mask from "opaque only" colour and the
   final pre-upscale colour. We can copy the scene target at the first
   blended, depth-write-off draw of the scene pass. That needs one measurement:
   whether GT5 draws its transparents after its opaques in one block
   (`RSX_CAP` manifests list blend state per draw).
3. **Draw classification in the recorder.** For scene draws with blending on
   and depth write off, also write `alpha` (clamped to 0.9) into an `R8_UNORM`
   target with max blending: one extra render target on those draws, the same
   way the MV pass adds one. This changes those pipelines, so it must enter
   the pipeline key and `fplk_epoch`.

The resulting `R8` texture serves FSR `reactive`, XeSS's responsive mask and
DLSS's optional bias-current-colour mask, so the ABI carries it once.

### 4.4 Depth

- Format: DLSS gets the `R24G8_TYPELESS` copy. XeSS accepts any depth format.
  Whether the FSR API's resource description accepts `R24_UNORM_X8` is
  **unverified**. Reuse `fg_depth_convert()` (`R32_FLOAT`, already written
  for DLSS-G) for FSR, and for XeSS if its R24 path misbehaves. It costs one
  full-screen pass at render size.
- Polarity: standard Z for all three: FSR without `DEPTH_INVERTED` (AMD
  recommends inverted, but standard works), XeSS without `INVERTED_DEPTH`,
  XeFG without `INVERTED_DEPTH`. GT5's far plane is finite, so no
  `DEPTH_INFINITE`. FSR FG and XeFG want `cameraNear/Far` and vertical FOV,
  which `fg_extract_camera()` already produces for DLSS-G.

### 4.5 Motion vectors

`s_mv_tex` is `R16G16_FLOAT`, the offset from the current pixel to its
previous position, in render pixels, with jitter removed. This is the native
convention of all three vendors (FSR: "current to previous, pixels,
`motionVectorScale` = (1,1)"; XeSS: "pixels, current to previous, without
jitter, low resolution"). So:

| Consumer | Scale |
| --- | --- |
| DLSS SR, FSR SR, XeSS SR | (1, 1), low-res (default for all) |
| DLSS-G | (1/w, 1/h) (as now) |
| FSR FG prepare | (1, 1) in pixels; check against the doc's `motionVectorScale` definition for FG |
| XeFG | pixels (no `USE_NDC_VELOCITY`) |

Neither FSR's `MOTION_VECTORS_JITTER_CANCELLATION` nor XeFG's `JITTERED_MV`
applies, provided the camera fill pass is also free of jitter. The per-object
part was verified in 0.3.1; check the camera fill on static scenery with
jitter on.

### 4.6 Jitter

Phase count: FSR's formula `ceil(8 * (display/render)^2)` is ours already
(Quality 18; Balanced 23 for 1.7, ours 24 for 0.58). XeSS uses its own
ratios; take the phase count from the plug-in.

Sign: FSR applies `+2*jx/W`, `-2*jy/H` to the projection and receives the same
`(jx, jy)`. XeSS's guide applies the same shear but states its effective
jitter is the negation. DLSS on our data needed `+1`. Wrappers that sit
between these SDKs usually pass DLSS jitter straight to FSR, so FSR is
probably `+1` too (hypothesis). XeSS is the likely place for a flip. Do not
guess: the plug-in reports a default sign, the backend passes `jit` raw in one
convention (the DLSS one we measured), and each plug-in converts. Re-run
the 8-frame static MAE test (0.124 against 0.385 for DLSS) for each vendor,
with `RSX_UPSCALE_JITTER_SIGN` as an override.

### 4.7 Mip bias

DLSS: `log2(r/d)`. FSR: `log2(r/d) - 1.0` (-1.58 at Quality). XeSS: the
guide gives its own formula (read it during Phase 2). The bias is baked into
the static sampler bank and so into pipelines. The plug-in should report an
offset, and the backend should quantise the final bias to a few values that
are part of the pipeline key and `fplk_epoch`. Otherwise changing vendor at
the same scale serves pipelines learned with the wrong bias.

### 4.8 Frame-generation swap chain and present

| | DLSS-G (ours, direct NGX) | FSR 3.1 FG | XeSS-FG |
| --- | --- | --- | --- |
| Who creates the swap chain | We do | FFX (`...ForHwndDX12` or `...WrapDX12`), returns `IDXGISwapChain4` | XeFG (`InitFromSwapChain` consumes ours, refcount must be 1) |
| Who presents generated frames | We do: `fg_present_extra`, back-to-back Presents on the present-replay thread | The proxy, from its own threads, paced | The proxy, from its own thread |
| Generation work | Our command list (`NGX_D3D12_EVALUATE_DLSSG`) | At Present, on the present queue or async compute | On the queue given at init |
| Per-frame inputs | `Pt5FgEval` (colour, depth R32, MV, hudless, camera, clip-to-prev) | Prepare V2 (depth, MV, jitter, camera, frameID) + configure (HUDLessColor, enabled, frameID) | Tagged resources + frame constants + present ID |
| HUD | hudless + backbuffer, NGX extracts | `HUDLessColor`, or UI texture/`presentCallback` composition | `BACKBUFFER_HUDLESS` (ours) or UI texture |
| Pacing | Ours: generated frame at sync 0, then the real frame at `s_psync`; `RSX_FG_PACE` opt-in sleep | Built in; VSync on: aim slightly above half refresh | Built in; any VSync/VRR; XeLL wants sync 1 |
| Latency | none (no Reflex) | Anti-Lag 2 optional, separate SDK | XeLL mandatory |
| Multi-frame | 2x to 4x (RTX 50) | 2x | `maxInterpolatedFrames` (multi-frame needs XeSS 3; limits not documented in what was read) |
| Exclusive fullscreen | n/a (we never use it) | VRR/tearing need fullscreen; borderless covering the screen counts for 4.0 pacing | FG cannot run there (we never use it) |

What changes in `rsx_d3d12_backend.c` for a proxy frame generator:

1. **Swap-chain creation becomes a plug-in hook.** At the
   `CreateSwapChainForHwnd` site, when the configured FG backend owns the
   swap chain, call `pt5_fg_create_swapchain(device, queue, hwnd, &sc_desc,
   &out)` and take `IDXGISwapChain3` from the result. This happens before the
   first frame, so **choosing FSR or XeSS frame generation is a restart
   setting**. Enabling it within the chosen backend stays live
   (`frameGenerationEnabled`, `xefgSwapChainSetEnabled`). DLSS-G (manual)
   keeps working on a plain swap chain.
2. **Present.** In proxy mode, `fg_eval_frame()` becomes "tag/prepare" only
   (record the prepare dispatch, or tag resources, into the frame's command
   list before `Close`). `fg_present_extra()`/`fg_present_resource()` are not
   used. `Present(s_psync)` happens once per real frame, as without FG.
   `g_rsx_fg_active_gen` and the F3 "effective FPS" come from the proxy's own
   statistics rather than our count.
3. **Frame ID.** One monotonic `frame_id` per real frame, used by prepare,
   configure and Present (FFX `frameID`, XeFG `SetPresentId`).
4. **Frames without a scene** (menus, movies, loading, Photo Mode, a reset):
   the NGX path just skips evaluation (`mv_ran` false). A proxy still sees
   every Present. Send `frameGenerationEnabled = false` (FFX) or skip tagging
   and disable (XeFG) on such frames, then send `reset` on the first scene
   frame. Toggling has a cost in FFX (VRR reset), so add hysteresis: re-enable
   only after N scene frames.
5. **F1 overlay.** Today it is inside the FG colour input. With proxies,
   simplest: no frame generation while the overlay is up. GT5 pauses races
   when F1 opens, and a paused scene gains nothing from interpolation. Later,
   FFX's UI texture or XeFG's UI texture could carry the overlay
   (premultiplied) so it is never interpolated.
6. **Output pass and sizes.** With `s_out_on` the swap-chain buffers are
   `s_out_bufs` at output size, and the hudless copy is taken before the
   output pass. Proxies want hudless with the backbuffer's exact size and
   format (stated for XeFG). The existing check (`hudless_w == out_w`) drops
   hudless when they differ. For proxies, capture hudless after the output
   scale instead (one more full-screen pass), or run without hudless.
7. **Resize and fullscreen.** We never call `ResizeBuffers` and never use
   exclusive fullscreen, which avoids the hardest proxy rules. Every
   `IDXGISwapChain*` call must go through the proxy pointer (XeFG says so
   explicitly).
8. **Device loss and shutdown.** `renderer_fail()` and shutdown must
   destroy in vendor order: FFX: disable FG, destroy the FG context, then the
   swap-chain context, then the last reference. XeFG: release swap-chain
   references, destroy XeFG, then XeLL. Test with `RSX_FAIL_TEST`.
9. **CPU.** FFX's pacer has one busy-waiting thread, and XeFG presents from
   its own thread. GT5 already uses about 5 cores with pinned guest PPU
   threads and pinned SPU lanes. The standing rule ("never let a new thread
   share the guest PPU CPUs") cannot be enforced inside a vendor DLL. Measure
   with `scripts/test/cpu3.ps1`. If the pacer lands on a guest CPU, set its
   thread affinity after creation by enumerating the process's new threads
   (hypothesis: feasible, ugly).
10. **Pacing of our own DLSS-G.** Presenting a generated frame at sync 0 and
    then the real frame at sync 1 is uneven by construction: the generated
    frame is shown as soon as the flip queue allows and the real one at the
    next vblank. A proxy will likely pace better than our DLSS-G does today.
    Compare both with PresentMon (`MsBetweenDisplayChange` spread). If FSR FG
    on an RTX 40 card paces visibly better, that is a reason to revisit
    DLSS-G pacing (Streamline or Reflex), not to drop it.

Separate SR and FG: yes. SR (any vendor) runs at the scene/HUD boundary into
`out_res`; FG (any vendor) consumes the backbuffer, hudless, `s_mv_tex`,
R32 depth and camera. They share nothing vendor-specific. Rules: one FG
owner per swap chain; both plug-ins on one `ID3D12Device`; each restores no
state, so the backend restores heaps and root signature after every external
call, as now. Useful pairings: DLSS SR + FSR FG on RTX 20/30; XeSS or FSR SR
+ FSR FG on AMD and Intel; DLSS SR + DLSS-G stays the RTX 40/50 default.

## 5. Plug-in ABI v2 and backend refactor

### 5.1 ABI (`libs/video/rsx_upscale_plugin.h`)

- `PT5_UPSCALE_ABI_VERSION 2`. The backend accepts 2. The DLSS plug-in is
  rebuilt to v2 in the same release (we ship them together). Optionally keep a
  v1 loader path for an old DLL dropped in by hand.
- **Discovery.** The backend loads `pt5_upscale_dlss.dll`,
  `pt5_upscale_fsr.dll`, `pt5_upscale_xess.dll` (fixed list, beside the exe;
  no wildcard loading of arbitrary DLLs). One entry point,
  `pt5_plugin_get(uint32_t abi, Pt5PluginApi* out)`, fills a table of
  function pointers plus:

  ```c
  typedef struct Pt5PluginInfo {
      uint32_t struct_size;
      char     id[16];          /* "dlss", "fsr", "xess" */
      char     name[64];        /* shown in F1 */
      uint32_t caps;            /* PT5_CAP_SR | _AA | _FG_MANUAL | _FG_SWAPCHAIN | _REACTIVE | _SHARPEN | _LATENCY */
      uint32_t n_modes;
      struct { int32_t mode; float scale; char label[24]; } modes[10];
      float    mip_bias_offset; /* 0 DLSS, -1 FSR, ... */
      float    jitter_sign;     /* default; backend override wins */
  } Pt5PluginInfo;
  ```

- **Contexts, not globals.** `create` returns a `void* ctx`, and `eval`,
  `destroy` take it. This lets SR and FG from one DLL coexist cleanly and
  allows recreate-on-resize without tearing down the other.
- **Eval v2** (adds to v1): `reactive` (R8, optional), `transparency`
  (R8, optional), `exposure` (1x1 R32, optional), `pre_exposure`,
  `color_space` (`Pt5ColorSpace`), `sharpness` (<0 = plug-in default off),
  `cam_near`, `cam_far`, `cam_fov_v`, `depth_fmt_hint`. Jitter is in the one
  measured convention (render pixels, as DLSS gets it now). Motion vectors
  are fixed to "previous minus current, render pixels, unjittered".
  `struct_size` keeps forward compatibility as in 0.3.1.
- **Frame generation v2**, two modes picked from `caps`:
  - Manual (DLSS-G now, maybe FFX standalone dispatch): existing
    `pt5_fg_create/eval/destroy` with contexts.
  - Swap chain (FFX proxy, XeFG):
    `pt5_fg_create_swapchain(device, queue, hwnd, const DXGI_SWAP_CHAIN_DESC1*, void** out_swapchain4, reason)`,
    `pt5_fg_frame(ctx, cmd_list, const Pt5FgEval*)` (prepare or tag; reuses
    the 0.3.1 struct: depth, MV, hudless, camera, jitter, frame ID, reset),
    `pt5_fg_set_enabled(ctx, int)`,
    `pt5_fg_before_present(ctx, uint64_t frame_id)` (FFX configure frame ID,
    XeFG `SetPresentId`),
    `pt5_fg_stats(ctx, Pt5FgStats*)` (presented and generated counts),
    `pt5_fg_destroy_swapchain(ctx)`.
- **Latency** (optional cap): `pt5_latency_sleep(frame_id)` and
  `pt5_latency_marker(type, frame_id)` so Reflex, Anti-Lag 2 and XeLL share
  one set of call sites. Placement in our frame (hypothesis, to measure):
  simulation start/end around the guest frame (between `SetPrepareFlip`
  calls), render submit around the FIFO walk and replay, present around
  `Present`.

### 5.2 Backend (`libs/video/rsx_d3d12_backend.c`)

- Rename `s_dlss` to a slot type used twice, `s_up` (super resolution) and
  `s_fgp` (frame generation), each pointing at a loaded plug-in. Rename
  `dlss_*` functions to `up_*`. `dlss_is_scene_rt`, the jitter, MV, depth,
  mip bias, scene-RT scaling, slot substitution, reset and fail-streak logic
  stay as they are: they are vendor-neutral.
- `dlss_scale_factor()` and the Halton phase count read the chosen plug-in's
  mode table and its `optimal()` result instead of constants.
- Environment (each with its old spelling kept as an alias):
  `RSX_UPSCALER=off|dlss|fsr|xess|auto`, `RSX_AA=native|q|b|p|u|c` (old
  `dlaa`/`dlss_*` still mean DLSS), `RSX_FG_BACKEND=auto|dlss|fsr|xess`,
  `RSX_FG=off|2x|3x|4x`, `RSX_UPSCALE_COLOR`, `RSX_UPSCALE_JITTER_SIGN`,
  `RSX_UPSCALE_REACTIVE=0|1|2` (none, opaque snapshot, classification).
- Fallback chain: requested backend, then `auto` (DLSS on RTX, XeSS on Arc,
  FSR elsewhere), then native MSAA; the reason is kept per backend for F1.
- `up_depth_r32` (moved from `fg_depth_convert`) shared by DLSS-G, FSR and
  XeSS.
- Proxy swap-chain path as in 4.8, under `RSX_FG_SWAPCHAIN_PLUGIN=0` to
  force the plain swap chain.
- Pipeline key and `fplk_epoch`: add the quantised mip bias and any reactive
  render target.

## 6. F1, settings, build, packaging, licences

### 6.1 F1 > VIDEO

| Row | Values | Live? |
| --- | --- | --- |
| Upscaler | Off (MSAA) / NVIDIA DLSS / AMD FSR / Intel XeSS / Auto; unavailable ones show the plug-in's reason ("needs RTX", "DLL missing", "SM 6.4 not supported") | Yes (as DLSS now) |
| Upscaler quality | the chosen plug-in's modes: Native AA (DLAA), Quality, Balanced, Performance, Ultra Performance; XeSS adds Ultra Quality / Ultra Quality Plus | Yes |
| Sharpening | Off / 0.1 to 1.0 (FSR RCAS; hidden for DLSS and XeSS) | Yes |
| Frame generation | Off / 2x / 3x / 4x (counts the backend supports) | Yes within a backend |
| Frame generation backend | Auto / NVIDIA / AMD / Intel; changing it says "restart needed" | Restart |

Settings file, `[video]`: `upscaler`, `upscale_mode`, `sharpness`,
`frame_gen`, `frame_gen_backend`. Read old `dlss = "quality"` as
`upscaler = "dlss"`, `upscale_mode = "quality"`, and write the new keys. Files:
`src/gt5p_settings.h`, `src/gt5p_settings.cpp` (`kDlssNames`/`kDlssEnv`,
`set_default_env`), `src/gt5p_settings_ui.cpp` (rows at lines 341-351,
`rsx_d3d12_set_dlss_mode` becomes `rsx_d3d12_set_upscaler(backend, mode)`).
Availability needs the device, so the backend publishes per-plug-in status
after init (`rsx_d3d12_upscaler_status(id, char* reason)`) for the UI to read.

### 6.2 Build and release

- `CMakeLists.txt` (DLSS block at lines 640-666): two more optional targets
  built only when their SDK folder exists:
  - `pt5_upscale_fsr` from `third_party/fidelityfx` (headers, loader import
    library, signed `amd_fidelityfx_*_dx12.dll` copied post-build);
  - `pt5_upscale_xess` from `third_party/xess` (headers, `libxess.lib`,
    `libxess_fg.lib`, `libxell.lib`, DLLs copied post-build).
  Both folders gitignored.
- `scripts/get_fidelityfx.py`, `scripts/get_xess.py`: download a pinned
  GitHub release by tag, check SHA-256, unpack the needed files, like
  `scripts/get_dxc.py`. Unlike the DLSS SDK both are public downloads, so a
  clean build can fetch them.
- `scripts/release/clean_build.py`: `--fsr-sdk`/`--no-fsr`,
  `--xess-sdk`/`--no-xess` mirroring `--dlss-sdk`/`--no-dlss`
  (lines 90-196), with the same "headers present or fail" check.
- `scripts/release/package_player.py`: allowlist entries for the plug-in
  DLLs, the vendor DLLs and their licence notices under
  `third_party_notices/`. Confirm whether the VC++ 14.40 runtime XeSS
  requires is already present on players' machines or must be shipped.
- `docs/release-av.md`: every new DLL goes through `vt.py scan`.
- `scripts/check_no_game_files.py` needs no change (vendor files are never
  staged), but add the two SDK folders to `.gitignore`.
- Tests: a **null plug-in** (`pt5_upscale_null.dll`, bilinear upscale in a
  compute shader, built from our source only) lets
  `scripts/test/upscale_plugin_abi.py` exercise discovery, fallback, state
  restore and ABI versions under WARP without any vendor SDK.

### 6.3 Licences (verify the exact text of each pinned release before shipping)

| | Licence | May we ship the DLL? | Conditions | Repo |
| --- | --- | --- | --- | --- |
| NVIDIA DLSS / DLSS-G | NVIDIA DLSS SDK licence (reviewed for v310.9.16 in plan-dlss.md) | Yes, as part of an application that adds material function | Attribution notice; terms as protective; never let the SDK become open-source licensed | Never commit |
| AMD FSR SDK 2.x (Redstone, 2.4.0) | AMD grant in `docs/license.md`: binary-form redistribution, no reverse engineering, notices reproduced; samples MIT; headers read carry MIT notices | Yes (signed DLLs are required by SDK 2.x) | Reproduce notice; no RE | Do not commit DLLs; headers probably could be, but fetch them like the DLLs for one rule |
| AMD FidelityFX SDK 1.1.x (FSR 3.1.x) | MIT with source (hypothesis: re-check the tag) | Could be built into our plug-in from source, no AMD DLL | MIT notice | Source could even be vendored |
| Intel XeSS (SR, FG, XeLL) | Intel Simplified Software License (Oct 2022) | Yes, binaries unmodified | Copyright and licence text with the software and its documentation; no endorsement; no RE or modification | Never commit |

Recommendation: FSR through SDK 2.x signed DLLs. It is the only route to
FSR 4 on RX 9000 and to driver-side upgrades, and it keeps one integration.
It is not plain MIT, contrary to the starting assumption. Note also that
0.3.1 ships `nvngx_dlss.dll` and `nvngx_dlssg.dll` in the ZIP (they appear in
its VirusTotal log), whereas plan-dlss.md recommended a launcher fetch. That
is consistent with the licence (§2(a)), but plan-dlss.md should be updated to
say what was actually chosen.

## 7. Phases

Each phase ends with measurements on 01.04 and 2.11 before the next starts.

### Phase 0: ABI v2, no visible change

- Files: `libs/video/rsx_upscale_plugin.h`, `libs/video/rsx_d3d12_backend.c`
  (slot refactor, plug-in table, `up_depth_r32`), `src/pt5_upscale_dlss.cpp`
  (v2 table, contexts), new `src/pt5_upscale_null.cpp`, `CMakeLists.txt`,
  `scripts/test/upscale_plugin_abi.py`; refresh
  `patches/ps3recomp-5244db9-local.patch` and check it applies to a fresh
  `5244db9`.
- Pass: `race.ps1 -FrameTest` with DLSS Quality and DLSS-G 2x matches 0.3.1
  within noise; an `RSX_CAP` backbuffer matches 0.3.1 (MAE about 0 on a static
  frame); `RSX_CMD_CHECK=1` 0 reports; WARP test passes with the null
  plug-in; a v1 DLL is refused with a clear reason.
- Risk: low; it is a refactor of code with good rollbacks.

### Phase 1: FSR Super Resolution and Native AA

- Files: new `src/pt5_upscale_fsr.cpp`, `scripts/get_fidelityfx.py`,
  CMake, settings and F1 rows, `clean_build.py`, `package_player.py`, notices.
- First measure: the scene target's colour space (4.1); the transparent
  ordering (4.3, for later); the jitter sign (MAE test); FSR's acceptance of
  R24 depth or use of R32.
- Pass: frame test at 1080p Quality and 1440p/4K Performance against DLSS and
  native on the RTX 4060; MAE jitter test; shots of High Speed Ring fences,
  wheels, mirrors, the tunnel (exposure), Eiger rain (ghosting), the car list,
  21:9; Native AA against DLAA; no `PSO FAIL`.
- Risks: ghosting on rain and smoke (Phase 2b), descriptor-heap state after
  FFX (restore exists), FSR 4 untested without an RX 9000 card (ask a player
  with one; FSR 3.1 is what we can verify).

### Phase 2: XeSS Super Resolution and Native AA

- Files: new `src/pt5_upscale_xess.cpp`, `scripts/get_xess.py`, CMake,
  settings, packaging, notices.
- Specific: `xessD3D12BuildPipelines` non-blocking at startup (otherwise the
  first enable stalls); scale table from `xessGetOptimalInputResolution`;
  mip bias formula from the guide; jitter sign (the likely flip).
- Pass: as Phase 1 on the RTX 4060 (DP4a path); ideally one Arc check by a
  player (XMX path).

### Phase 2b: reactive/responsive mask (shared)

- Order: none, then the opaque snapshot with FSR's generator, then draw
  classification (4.3). Pipeline key and `fplk_epoch` if a render target is
  added. Rollback `RSX_UPSCALE_REACTIVE=0`.
- Pass: Eiger heavy rain and a tyre-smoke start show less trailing than
  Phase 1 in side-by-side captures; frame test cost under 0.3 ms.

### Phase 3: FSR Frame Generation

- 3a (cheap probe): try `ffxDispatchDescFrameGeneration` alone through the
  existing manual path (`fg_present_extra`). If AMD permits it and it
  works, FSR FG ships with no swap-chain change, using our (weak) pacing.
- 3b (supported route): proxy swap chain (4.8, points 1-9). Files:
  `rsx_d3d12_backend.c` (creation hook, present, frame ID, enable/disable
  hysteresis, shutdown order), `pt5_upscale_fsr.cpp` (swap-chain and FG
  contexts), settings (backend restart row).
- Pass: PresentMon over 60 s of High Speed Ring: display-change interval
  spread at 2x, against DLSS-G 2x on the same card; the real frame rate from
  the frame test unchanged within noise; `cpu3.ps1` shows no pacer thread on
  guest CPUs; menus and movies show no interpolation artefacts; F1 open and
  close with FG on; `RSX_FAIL_TEST` device loss with the proxy; 21:9 and the
  output pass (`s_out_on`) with hudless; DLSS SR + FSR FG pairing; the
  30 FPS rolling start (FSR 3.1.6 wants 60+; use 3.1.7 or disable below a
  threshold).
- Risks: pacing busy-wait competing with SPU lanes; the hudless size rule
  with the output pass; enable/disable cost on menu transitions; HAGS off.

### Phase 4: XeSS-FG (only if wanted)

- Needs XeLL with markers that fit a recompiled guest (5.1 latency hooks),
  `InitFromSwapChain` with refcount 1 on our swap chain, sync interval 1, and
  agreement with GT5's own vblank pacing (the guest's vblank thread,
  `GT5P_VBLANK_HZ`). Intel's own documents disagree about non-Intel support.
- Recommendation: do not start until Phase 3 is done and someone with an Arc
  card asks for it. FSR FG already covers Intel GPUs.

## 8. Open questions

- Colour space of the scene target (4.1).
- FSR acceptance of `R24_UNORM_X8` depth (4.4).
- Whether AMD supports `ffxDispatchDescFrameGeneration` without the proxy
  (Phase 3a).
- XeSS jitter sign and mip-bias formula on our data.
- Exact licence text of the pinned FSR SDK 2.x and XeSS releases; whether
  FidelityFX SDK 1.1.x is MIT throughout.
- XeSS-FG on non-Intel GPUs: guide and README disagree.
- Whether GT5 draws its scene transparents after its opaques in one block.

## Sources (vendor documents read 2026-11-26)

- AMD FSR upscaler: https://gpuopen.com/manuals/fidelityfx_sdk2/techniques/super-resolution-upscaler/
- AMD FSR frame generation swap chain: https://gpuopen.com/manuals/fidelityfx_sdk2/techniques/frame-interpolation-swap-chain/
- AMD FSR frame generation: https://gpuopen.com/manuals/fidelityfx_sdk2/techniques/frame-interpolation/
- AMD SDK repository, README, `docs/license.md`, `ffx_framegeneration.h`, `ffx_upscale.h`: https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK
- AMD FidelityFX SDK 2.0 announcement: https://gpuopen.com/learn/amd-fidelityfx-sdk-2-0/
- Intel XeSS repository (README, LICENSE.txt, SR and XeLL guides): https://github.com/intel/xess
- Intel XeSS-FG developer guide: https://www.intel.com/content/www/us/en/developer/articles/technical/xess-fg-developer-guide.html
- XeSS 2.1 non-Intel FG and XeLL: https://www.techpowerup.com/339492/intel-opens-up-xess-2-to-nvidia-and-amd-gpus-with-sdk-2-1-0
