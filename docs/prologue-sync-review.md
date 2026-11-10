# Tourline Prologue review and GT5 ports, 2026-11-11

Compared Project Tourline 5 `94d6fac` with Tourline Prologue **v0.2.5,
`7d0ab12`**, fetched from its GitHub origin. Their common ancestor is
`7ca5059`. The source range contains **236 commits, 218 excluding merges**.
The local Prologue checkout was still at v0.2.4; the review includes the
newer remote commits through the v0.2.5 publication.

This is a selective source port. A merge of the complete Prologue tree would
replace GT5's runtime compatibility patch, player instructions and branding,
and would introduce executable-specific preparation metadata and hooks.

## Adopted

| Change | Source | GT5 adaptation and rollback |
| --- | --- | --- |
| Partial analog trigger pressure survives the digital threshold | `03c5712` | Applied to real `cellPadGetData`. `PAD_PRESS_FORCE_FULL=1` restores full pressure for every digital press. |
| PPU join waits for the host epilogue before freeing its slot | `3a70ace`, `805f4f7` | Retained GT5's thread implementation. `PS3_PPU_JOIN_WAIT_HOST=0` restores the race. |
| Trigger dead zone and linear/gentle/quick curves | `ec53908` | F1 Controls; `input.trigger_deadzone` (0â€“20), `input.trigger_curve` (0â€“2). Zero/linear defaults preserve existing response. `PAD_CONTROLS_POLISH=0` bypasses shaping and optional feedback. |
| Optional SDL body vibration, Xbox trigger motors and fixed DualSense resistance | `ec53908`, `79ecafc` | F1 Feedback; all off by default. `PAD_SDL_RUMBLE`, `PAD_TRIGGER_RUMBLE`, `PAD_ADAPTIVE_TRIGGERS`. Device handles stay on the SDL owner thread; focus/poll loss stops effects. These forward existing motor commands or apply fixed resistance, without vehicle telemetry. |
| Correct capability response size | `79ecafc` | Writes 128 bytes instead of 256. `PAD_CAPABILITY_INFO_FIX=0` reproduces the overwrite. |
| Input-runtime prerequisites | Prologue v0.18 compatibility/input implementation | Dynamic System32 XInput loading, neutral disconnected pads, serialized polling, connection-change reporting and rumble lifetime handling. Preserved GT5's hashed-file milestones, file log, `pad_milestone_mark`, 256-event scripts and overflow warning. |
| Smaller fragment register declarations | `c0d8888` | Sizes arrays to the highest literal index; uncertain indices keep 48 entries. `RSX_FP_REG_SIZE=0` restores the old text. |
| Looped GPU vertex fetch | `c0d8888` | `RSX_VFETCH_LOOP=2` default; 0 restores original fetch, 1 loops components only. GT5's other vertex translation stays intact. |
| Share fragment text across sample counts | `5272a3d`, `802fcd4` | The actual pipeline sample count stays unchanged. `RSX_FP_SAMPLE_SHARE=0` restores separate single-sample text; `RSX_SSAA=0` still selects pixel shading. |
| Omit unused gamma helpers and redundant constant-mask encoding | `c0d8888` | Preserved GT5 texture/gamma handling. `RSX_FP_GAMMA_LAZY=0`, `RSX_FP_MASK_ENCODE=0` restore the previous text. |
| Demand and preload indicators counted separately | `5f712a1`, `84006c3` | Compiling shaders indicates demand work. Optional Loading shaders badge: `video.preload_indicator`, `GT5P_PRELOAD_INDICATOR=1`. `RSX_SHADER_INDICATOR_LEGACY=1` merges counters again. Snapshots report both. |
| Preload default enabled for new settings | `5f712a1` | Existing explicit `shader_warmup=false` remains false. Deliberately omitted Prologue's version-based migration that overwrites older false values. `RSX_SHADER_WARMUP=0` overrides the new default. |
| Source-keyed shader bundle | `ba4aeb4` | `RSX_DXBC_BUNDLE=0` restores per-file access; original shader files and packs remain. Shared format writer/checker: `scripts/dxbc_bundle.py`. CRC initialization uses `InitOnce`; shutdown joins importers before closing storage. `RSX_DXBC_BUNDLE_IMPORT=0` disables background import. |
| Process scheduling headroom | `ba4aeb4` | Above-normal process class during play, preserving relative guest priorities. Offline preparation is excluded. `GT5P_PRIORITY_CLASS=normal` restores the old class; `high` is also explicit. No claim that this fixes GT5 Phase 32's freeze. |
| Optional texture LRU policy | `4d85c9c` and its overlay | Kept **off**, unlike the later Prologue default. `RSX_TEX_LRU=1` opts in. Empty slots and current-frame resource protection still win. |
| Bounded hitch tracing and readers | `4d85c9c`, later trace overlay; `ba4aeb4` | `RSX_HITCH_LOG=<fresh CSV>` records up to 32,768 batches; reader `scripts/diag/frame_hitches.py`. Adapted around GT5 border-cube decoding and offscreen-present guard. Added read-only thread CPU tools, targeting `gt5.exe`. |
| Runtime isolation and reproducibility | `0d9f1a6`, `fea64b8`, subsequent setup fixes | Added manifest/setup/check workflow, retaining **GT5's 5244db9 base** rather than upgrading it. Default `../ps3recomp-tourline5`; `PS3RECOMP_DIR` overrides build, HLE/SPU lift and test paths. Patch bytes are preserved on Windows. |
| Absolute game-directory metadata reads and offline-output flush | launcher integration; `41b92c8` | Direct PARAM.SFO reads now agree with the VFS absolute-path mapping. Offline completion flushes before exit (`GT5P_OFFLINE_FLUSH=0` rollback). |
| F1 key toggles the settings page | v0.2.5 UI | Adopted independently of Prologue's native controls and launcher tool rows. |

Shader text changes produce new source keys. A first visit may compile new
shaders; the old cache is retained. There is no wholesale cache invalidation
or deletion, and Prologue's shader catalogue is not installed into GT5's cache.

## Reviewed and deferred

The remaining commits fall into the following groups. Some include generic
components, but their complete behavior is not verified for this executable.

| Group | Why it was not enabled here / next prerequisite |
| --- | --- |
| Full ps3recomp v0.18 migration (`0d9f1a6`) | The replacement patch omits GT5's pager/mmapper, syscall, SPURS, DMA, movie and FIFO changes. Requires a separate three-way runtime migration, lifter audit/re-lift and GT5 boot/install/movie/race acceptance. |
| Prologue material reconstruction, shader-first enumeration, input-only templates and its starter catalogue (Phases 137â€“216) | Material layouts, shader locations, target identity, executable checksums and measured emit paths belong to BCUS98158. Re-derive these for BCUS98114-v104 before producing a GT5 catalogue. Shipping those identifiers here would not establish valid GT5 coverage. |
| Prologue player launcher, storage migration, release ZIPs, RecompLauncher manifest and CI (Phases 135â€“220 and later UX) | Installer/title/version validation, payload names, assets and release infrastructure target Prologue. Keep this repository's PT5 identity and current source-build instructions. A GT5 player release needs its own verified update/install workflow. |
| Community pipeline collection, offline proof and contribution UI | Depends on the Prologue catalogue, preparation helper and managed launcher lifecycle. No nonfunctional collection or preparation buttons were added. |
| Native Modern controls and Opening Movie links (`1c7cb3e`, `4dd582f`) | Their measured functions, vtables and profile layout belong to Prologue. GT5 settings addresses must be measured. Host trigger shaping/feedback work independently; existing GT5 opening behavior remains. The modern-row null-name crash (`d872f39`) occurs in that deferred UI, so its unsafe row logic was not introduced. |
| Generic shader interpreter and adaptive budget (Phases 216/220/224) | Depends on newer backend resource layouts, prepared interpreter classes, embed/build tools and broader parity checks. Prologue itself documents incomplete parity and performance limits. |
| DXC / SM6 compiler migration, translation modes and cleanup offers (Phases 227/231) | Requires a DXC dependency/package policy, GT5 shader parity and independent cache namespaces/preparation contracts. FXC remains the compiler. No old-cache cleanup is performed. |
| Placed textures, texture allocation/staging pools, fast PSO lookup, parallel race preparation and the newer recipe pack (Phase 221/228) | These interact with GT5's border cubes, copy/volatile texture lifetimes, async queue, pipeline descriptors and render-thread ownership. Retain those bespoke paths until adapted and GPU-validated. The source-keyed bytecode bundle above is independent and was ported. |
| Background/quick preparation, translated membership, loading bar and deferred launcher notifications (Phases 223â€“236) | Depends on the catalogue/helper/launcher and newer race gate counters. Existing GT5 preparation remains. The separate background-preload badge is available. |
| Release bumps, Prologue READMEs/changelogs, publication proofs, route sweeps and historical diagnostics | Retained as source evidence, not copied as statements of GT5 support or performance. Only independent diagnostics with functioning GT5 backends were adopted. |

## Preservation and reproducibility

The original shared runtime at `../ps3recomp-gt5p-master` was not edited.
The isolated runtime's patch preserves all original sections except the seven
files needed for the ports: input C/header, a log-counter helper header, the
D3D12 backend, both shader decompilers, and PPU thread implementation.
The other 76 existing patch sections are retained byte for byte from the starting commit; the input header and log helper are two newly added sections.
GT5 target constants, SPU native hooks/registries, generated code, install
layout, FIFO routing/park timeouts and rendering fixes remain in place.

To reproduce the comparison:

```text
git fetch https://github.com/mathewdouglas/Tourline-Prologue.git main
git log --reverse --no-merges 7ca5059..7d0ab12
git diff --stat 7ca5059..7d0ab12
.venv\Scripts\python scripts/setup_runtime.py
.venv\Scripts\python scripts/setup_runtime.py --check
scripts\build.cmd -DGT5P_TARGET=BCUS98114-v104
```

## Validation

- Release `build/gt5.exe` builds against the isolated runtime. The generated
  PPU/SPU code was rebuilt, not re-lifted; no generated code is committed.
- Main patch applies to a clean 5244db9 checkout and `setup_runtime.py --check`
  validates the installed result.
- `ppu_join_lifetime.py`: original and rollback paths reproduce premature
  slot reuse; corrected path passes 128 deliberately delayed host exits.
- `controls_ports.ps1`: 16,128 trigger-response cases, 255 real guest pressure
  packets, all-seven-port capability canaries, feedback API spies, timed
  stop/focus/disconnect/failure paths, virtual SDL reconnects and rollbacks.
  Physical Xbox/DualSense effects still require hardware acceptance.
- `prologue_ports.py`: 1,024 pressure values/bit combinations per mode,
  injected digital input, register bounds/conservative fallbacks and sample
  qualifier masks, in default and rollback modes.
- `vertex_fetch_ports.py`: 17 synthetic vertex programs per comparison,
  component-loop and full-loop variants against the original, 3,899,392 WARP
  output values total, zero bit differences. This covers fetch semantics,
  not an exhaustive GT5 shader corpus.
- `fp_source_range.ps1`: 240 rendered lanes per policy, all four policies
  pass with the sized-register emitter. Volume layout and 32 WARP volume
  lookups also pass.
- Production PSO queue/WARP tests pass, including loading gates, demand
  priority, preload/demand separation, bounded source/resident budgets,
  damaged recipes, cancellation, timeout and shutdown.
- Python/native shader-bundle round trips, append/read, missing key,
  corrupt-record rejection/repair and disabled/offline modes pass.
- Settings/export integration includes new control value validation,
  environment precedence, round trips and explicit preload-off preservation.
  F1 game-window checks exercise Video, Shaders, Controls, Feedback and Debug;
  control and feedback choices save correctly. Captures were inspected.
- Actual GT5 720p High Speed Ring smoke: main menu â†’ Arcade â†’ TrackList â†’
  CarList â†’ Transmission â†’ RaceLoad â†’ RaceStart, followed by 40 seconds of
  window captures. Road, cars, HUD, names and rear-view render. The bundle
  imported about 82 MiB from the existing cache while the game ran.
- The opt-in hitch trace produced 5,640 batches and its reader parsed every
  complete row. This run included new shader compilation and was **not a
  controlled performance comparison**, full-race finish or proof of fixing
  the existing intermittent 2.5-second freeze.

Local evidence is under `scratch/prologue-sync/`, `scratch/pv/prologue-sync-race.*`
and `scratch/settings_t/`; these contain private diagnostics and are ignored.
