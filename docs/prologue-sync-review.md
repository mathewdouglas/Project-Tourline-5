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
| Optional texture LRU policy | `4d85c9c` and its overlay | Kept **off** at first, unlike the later Prologue default. Default on since GT5 Phase 47 after measurement (`RSX_TEX_LRU=0` rollback). Empty slots and current-frame resource protection still win. |
| Fast pipeline lookup, learned in play (Phase 221) | `59cf049`, `6ffde70` | GT5 Phase 47: `RSX_PSO_FAST_LOOKUP` default on (`=0` off, `=2` check mode). GT5 tag: compiler, guest root signature, ultrawide, render height, high precision, `RSX_`/`FP_`/`VP_` values. The offline table writer (`fast_lookup_table.py`, `0d157bb`) is not ported: GT5 has no preparation helper. |
| Placed textures and staging ring (Phase 221) | `aa98847`, `6ffde70` | GT5 Phase 47: `RSX_TEX_PLACED`, `RSX_TEX_RING_MB` default on, beside GT5's own texture/staging pool (Prologue's `RSX_TEX_POOL` not ported; GT5 Phase 29 has its own). Released placed ranges return to the heap after the next GPU wait. Test `texture_heap_alloc_native.py` ported with deferred releases. |
| Bounded hitch tracing and readers | `4d85c9c`, later trace overlay; `ba4aeb4` | `RSX_HITCH_LOG=<fresh CSV>` records up to 32,768 batches; reader `scripts/diag/frame_hitches.py`. Adapted around GT5 border-cube decoding and offscreen-present guard. Added read-only thread CPU tools, targeting `gt5.exe`. |
| Runtime isolation and reproducibility | `0d9f1a6`, `fea64b8`, subsequent setup fixes | Added manifest/setup/check workflow, retaining **GT5's 5244db9 base** rather than upgrading it. Default `../ps3recomp-tourline5`; `PS3RECOMP_DIR` overrides build, HLE/SPU lift and test paths. Patch bytes are preserved on Windows. |
| Absolute game-directory metadata reads and offline-output flush | launcher integration; `41b92c8` | Direct PARAM.SFO reads now agree with the VFS absolute-path mapping. Offline completion flushes before exit (`GT5P_OFFLINE_FLUSH=0` rollback). |
| F1 key toggles the settings page | v0.2.5 UI | Adopted independently of Prologue's native controls and launcher tool rows. |
| Native setup, storage selection/migration and game-file pickers | Prologue launcher, adapted in GT5 Phase 37 | PT5-specific title/version/ELF checks, update-data import, temporary-copy RPCS3 decryption, remembered custom storage and managed restarts. Source game files and old storage are retained. See [setup](setup.md). |
| Optional DXC / shader model 6.0 guest compilation | Phases 227/231, `7d0ab12` | Follow-up GT5 Phase 34: pinned compiler/validator, F1 selection, separate cache namespace, offline preparation and native/GPU checks. `RSX_SHADER_COMPILER=fxc` rolls back. DXC became the default in release 0.1.0; randomized GT5 parity cases remain open; see [DXC](dxc.md). |

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
| RecompLauncher manifest, launcher tool pages and CI integration | Native setup/storage is now adapted in GT5 Phase 37, and release packaging is available. Prologue-specific managed-launcher metadata, preparation/contribution pages and CI still need GT5 integration; no unsupported preparation buttons were added. |
| Community pipeline collection, offline proof and contribution UI | Depends on the Prologue catalogue, preparation helper and managed launcher lifecycle. No nonfunctional collection or preparation buttons were added. |
| Native Modern controls and Opening Movie links (`1c7cb3e`, `4dd582f`) | Their measured functions, vtables and profile layout belong to Prologue. GT5 settings addresses must be measured. Host trigger shaping/feedback work independently; existing GT5 opening behavior remains. The modern-row null-name crash (`d872f39`) occurs in that deferred UI, so its unsafe row logic was not introduced. |
| Generic shader interpreter and adaptive budget (Phases 216/220/224) | Depends on newer backend resource layouts, prepared interpreter classes, embed/build tools and broader parity checks. Prologue itself documents incomplete parity and performance limits. |
| Shader translation modes and cleanup offers (Phases 227/231) | DXC is implemented and became the default in 0.1.0; [DXC](dxc.md) retains the parity findings. Interpreter modes and old-cache cleanup remain deferred. No old cache is deleted. |
| Placed textures, texture allocation/staging pools and fast PSO lookup (Phase 221) | Ported in GT5 Phase 47 (see Adopted), adapted to GT5's border cubes, volume textures, texture pool and pipeline key. Prologue's own `RSX_TEX_POOL` is not ported: GT5's Phase 29 pool already covers it. Parallel race preparation and the race pack from the same phases were ported earlier: see "Follow-up" below. |
| Prepared pipelines at the player's MSAA count (Phase 223, `fdc23e7`) | Measured in GT5 Phase 47: GT5 loses its warm race set only for the first race after an MSAA change, because membership is learned in play. Prologue's fix is in its offline preparation helper, which GT5 lacks; not ported. |
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

## Follow-up: race pack and helper-thread race preparation, 2026-11-11

Branch `port/deferred-prologue-improvements`, from `8b6f586`. One deferred
group was taken: Prologue Phases 221 (helper threads), 228 (race pack) and
229 (menu-time warmup reads the pack). It was chosen because it is confined
to the pipeline queue between the `PSO_ASYNC_BEGIN`/`END` markers plus two
small helpers above it. It does not touch texture, copy, FIFO, SPU or pager
code, and it creates pipelines only through the creation function the worker
already used.

**Not validated in the game.** No game or GPU run was made for this branch (a
parallel session was using them). Everything below is measured on synthetic
pipelines with D3D12 WARP. `gt5.exe` was not linked: the patched backend was
compiled to an object file with the build's own flags. The live checks still
needed are listed at the end.

### Adopted

| Change | Source | GT5 adaptation and rollback |
| --- | --- | --- |
| Race set prepared on helper threads during the loading gate | Phase 221 | `RSX_RACE_PREWARM_THREADS` (1..8, default 4). `=1` restores one recipe per worker turn. The pso-worker keeps serving demand jobs. A new load or shutdown stops helpers between recipes. |
| One file for the race set, `warmup-v1/race-pack-v1.bin` | Phase 228 | `RSX_RACE_PREWARM_PACK=0` neither reads nor writes it. `RSX_RACE_PREWARM_PACK_WRITE=0` reads an existing pack but never writes one. The renderer writes it after a load whose pack was missing or stale. |
| Menu-time warmup takes its race phase from the pack | Phase 229 | Same switches. Still one pipeline per worker turn with the 1 ms pause. |
| Preparation time split and gate progress counters | Phases 221, 230 | Printed with `[race-prewarm] finished` and in the F8 report line `Race prewarm:`. `g_rsx_race_load_done/total/waiting` are exported for a later progress bar. |
| Python pack reader/writer | Phase 228 | `scripts/race_preload_pack.py check|build <shadercache>`. Optional: the renderer writes the pack itself. |

### Where this differs from Prologue's code

- **The pack bytes are released after use.** Prologue keeps the file's bytes
  (150 to 250 MB for a full set) for the rest of the run. Here the worker
  drops them once the load or the warmup's race phase finishes, since those
  pipelines are then resident; the next load reads the file again. This
  machine has little free memory in a race (Phase 104). `RSX_RACE_PACK_KEEP=1`
  keeps them.
- **Repeated index lines no longer defeat the pack.** `race-v1.txt` can list a
  hash twice until the next compaction. Prologue's membership check then
  rejected the pack on every load and rewrote it. Index reads for the pack and
  the helpers now keep each hash once.
- **A stale pack repairs itself.** If the driver refuses a packed blob (as
  after a driver update) or a member cannot be served, that member goes
  through the per-file cache, and the pack is rewritten once in that run.
  Prologue kept using the stale bytes for as long as the index was unchanged.
- **No pack is written from a cancelled load or at shutdown,** and the writer
  stops between pipelines when the game exits. It writes the tables and then
  the payload, instead of building a second copy of the file in memory.
- **`RSX_PSO_DISK=0` also turns the pack off,** so that switch still means no
  driver blob is read from disk.
- **The thread count is read at each load,** not cached, so a test can change
  it. The helper that finishes last releases the gate; a start in which
  helpers finish before the starter does is handled explicitly.
- **GT5's own code is kept:** `create_preload_pso_cached` (which saves and
  restores the preload flag), the recipe hash check that accepts keys from
  before the root signature joined them, the resident cap of 8192 and
  `RSX_PSO_RACE_EVICT`.

### Not taken from the same phases

| Item | Finding |
| --- | --- |
| Loading bar under the logo (Phase 230) | Its position is measured against Prologue's loading screen. The counters it reads are ported; the bar needs a GT5 loading-screen measurement. |
| Preparation-time pack writer hook, translated membership lists | Depend on Prologue's preparation helper and catalogue, which are deferred above. |
| `RSX_TEX_POOL`, placed textures, staging ring | Texture resource lifetimes; unchanged. |
| Persistent fast pipeline lookup | Keyed on the renderer's in-memory pipeline key and an engine epoch. GT5's key fields differ; not examined further in this pass. |

### Validation (synthetic, WARP)

- `scripts/test/race_prewarm.py` compiles the real backend code from its
  hashing helpers to the end of the queue: hashing, driver-blob cache, recipe
  files, queue, pack and helpers. Nothing below the driver is a stand-in.
  48 pipelines sharing 4 shaders. It passed in 6 of 6 consecutive runs:
  - both rollback switches together keep the old path and write nothing;
  - four helpers prepare the set from files and the pack is written;
  - with every recipe, bytecode and blob file moved away, the pack alone
    prepares all 48 with 48 cached-blob driver calls, and its bytes are freed;
  - a reordered index with repeated lines still hits, in the live order;
  - a changed index, a flipped byte, a truncated file and a bad header are
    each rejected, files serve the load, and the rewrite reproduces the
    original bytes;
  - a refused driver blob and a damaged packed recipe each fall back for that
    member only;
  - the menu-time warmup prepares the race phase from the pack alone, and the
    later gate makes no driver call;
  - a demand job completes beside eight helpers; 20 overlapping reloads all
    finish; 20 shutdowns in the middle of a load return within the limit
    with no waiter or thread left.
- The Python tool reads the renderer's pack and rebuilds it byte for byte
  from the same files, and rejects the same damage.
- `scripts/test/pso_async.py` still passes all seven groups. It now pins
  `RSX_RACE_PREWARM_THREADS=1` and `RSX_RACE_PREWARM_PACK=0`, so it covers the
  rollback path; its ordering checks assume one worker.
- The refreshed patch applies to a clean 5244db9 clone and reproduces the
  private runtime tree exactly (`setup_runtime.py --check` passes on both).
  Only the `rsx_d3d12_backend.c` section changed; the other 82 sections are
  byte-identical to `8b6f586`.

### Live result on GT5

Run on GT5 afterwards (GT5 Phase 36 in [progress-gt5.md](progress-gt5.md)).
The race gate had never fired on GT5, because it keys on Prologue's `.gpb`
screen names; GT5 screen files were measured and wired in first. With that:
four helpers prepared 775 learned race pipelines in 640 ms from files and
the pack was written (35.9 MB); a later load hit the pack and prepared 781
in 93 ms after a 17 ms read, none rejected; the race picture is correct.

Still open: GT Life and other modes (not measured), DXC, working set with a
larger set, and whether the gate ever blocks long enough to notice.
Prologue's loading bar is not needed: GT5 draws its own, and the gate is
under a second here.

### Merged with the DXC work

Merged with `7953c71` (optional DXC). The runtime patch was rebuilt by
applying `patches/incremental/race-prewarm-pack.patch` to a runtime made from
the DXC patch; it applied without changes. The pack and its index sit in the
`warmup-v1` directory of whichever cache the compiler selects
(`shader_cache_dir()`), so FXC and DXC each get their own pack and neither
reads the other's. Driver blobs are in the directory above it in both cases.
The pack stores bytecode without interpreting it. Both queue tests pass on
the merged runtime with FXC; the pack was not exercised with DXC bytecode.

## Follow-up: GT5 CPU paths (Phase 40)

GT5 now supplies its SPU-progress wait and audio image identities rather than
leaving Prologue hooks inert. Audio uses the existing CPU/OS-gated FMA path;
GT5's measured vector render body has a separate opt-in, fingerprint-checked
FMA copy. Native clears use bulk writes with actual lifted-loop parity tests.
The measured HSR render thread never calls usleep while racing, so no
frame-pacing sleep was bypassed. See [Phase 40](progress-gt5.md) for tests,
live coverage and the render-copy acceptance still owed. The runtime
compatibility patch was not changed.
