# Changelog

All notable changes to this port. Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/);
versions follow [SemVer](https://semver.org/).

## [Unreleased]

### Changed
- **Faster integer SPU helpers.** SIMD forms of integer bitwise operations,
  control masks and shifts reduced CPU work in matched short-route tests:
  median FPS +3.7% at High Speed Ring and +5.6% at rainy Eiger. Eiger's p99
  frame time did not improve. CMake `GT5P_SPU_CODEGEN_HELPERS=OFF` rolls back;
  larger register-local/calling-convention experiments remain off.
- **Pipelines for a new place are built on several threads.** Draws waiting
  for a pipeline were served by one thread; up to three helper threads now
  share the work (kept off the game's own CPUs), the pipelines the most draws
  wait on go first, and the queue holds 1024 instead of 256. In cold-cache
  tests the interpreter stands in for far fewer draws. Switches:
  `RSX_PSO_DEMAND_THREADS=0`, `RSX_PSO_PRIORITY_USES=0`,
  `RSX_PSO_DEMAND_AFFINITY=0`.
- **Shader compiling gives way to the game during races.** While racing
  (and while the interpreter draws for new shaders), shader and pipeline
  building runs at idle priority, so it uses only CPU time the game leaves
  free: shorter worst frames on a first visit to a track. Menus and loading
  keep the normal priority. `RSX_SHADER_WORKER_PRIO_RACE=same` turns it off.

### Fixed
- **Course Maker's flickering map panel.** A partial display clear now clears
  its rectangle in order instead of presenting half a frame. Checked at
  720p/16:9; ultrawide map placement and clipped labels remain open.
  `RSX_SCISSORED_CLEAR=0` restores the old path.
- **Arcade car list: the frame-rate dip when a car appears.** The page's
  render work ran on one of its four SPU lanes for the first 8-14 frames
  with a car (the others had been parked while the list was empty), at
  55-68 ms a frame. Parked lanes now return as soon as the work needs them:
  one slow frame instead of up to 14. `SPURS_LANE_RECALL_US=0` restores the
  old behaviour.
- **Pauses while a menu or race loads for the first time.** Frames that
  draw one image at several growing sizes rebuilt it at every size, copying
  its contents each time: 10-59 rebuilds and up to a quarter of a second
  in one frame. Each image is now made once at its final size. Cold-boot
  loading: worst frame setup 105-286 ms -> 52-54 ms. `RSX_RT_FRAME_MAX=0`
  restores the old behaviour.
- **21:9: the opening WARNING, copyright and Polyphony screens are no
  longer stretched.** They are 1920x1080 pictures; at 720p the ultrawide
  pillarbox only recognised pictures exactly the display's size. Any 16:9
  picture at least display-sized now counts. `RSX_ULTRAWIDE_PILLAR=1`
  restores the exact-size rule.
- Crash and debug reports name the release by its version and commit
  (0.1.4's said `v0.1.3-66-...`: the build preceded its tag).
- **Hitches when other programs are busy.** The game's frame thread ran at
  Windows' lowest priority (idle), so a browser, a virus scan or the game's
  own shader compiling could take its CPU for hundreds of milliseconds. It
  and three threads of equal or higher PS3 priority now run at "lowest"
  instead, still below every other game thread. Under background load: race
  stalls 2-15 -> 0, worst frame 54-116 -> 44-47 ms; unchanged on an idle PC.
  `PS3_PPU_PRIO_LOW=idle` restores the old priority.
- Learned pipelines are no longer dropped on a busy first visit, so the next
  visit prepares all of them (`RSX_PSO_LEARN_BATCH=1` restores the old pace).

## [0.1.4] - 2026-11-14

Sound is back, setup needs only your game folder, and the first visit to a
menu or track stutters far less. Built with the same clean release pipeline,
now profile-guided. The player ZIP contains no game files; you need your own
extracted BCUS98114 disc (the folder containing `PS3_GAME`).

### Highlights
- **Menu music and race sound play again.** Players heard silence outside
  the opening movie.
- **Setup takes only your disc folder.** No RPCS3 install, no update
  download, no clicks: the launcher does the rest.
- **Far fewer missing details and hitches on first visits**, with a new
  "Draw while compiling" option, on by default.
- **Faster:** about +6% FPS from profile-guided builds, faster race starts
  and about 10% more frames in the Arcade car list.

### Added
- **Disc-only setup.** Select the extracted disc folder and nothing else.
  The disc's own `INSDIR/DATA000.PKG` already holds the required 01.04
  update. Setup downloads one pinned official RPCS3 build (37 MB, SHA-256
  checked), uses it only to install that package and decrypt its
  executable, then deletes it. It presses RPCS3's Install button itself; an
  RPCS3 window opens and closes on its own within seconds. Your own
  `rpcs3.exe`, or an existing 01.04 installation, can still be used instead.
  No keys are included. `PT5_RPCS3_DOWNLOAD=0` turns the download off; see
  [docs/setup.md](docs/setup.md) for the other switches.
- **RecompLauncher Easy Setup needs only the disc folder.** The RPCS3 and
  installed-data inputs are now optional.
- **Draw while compiling** (F1 > Shaders, and in RecompLauncher; on by
  default; `video.shader_interp`). While a new shader compiles, a general
  shader-interpreter draws in its place instead of the detail being left
  out. On a cold shader cache, about 90% fewer draws are left out and the
  worst frame is about 25% shorter. Off restores the old behaviour.
- **Problem reports.** After a crash, a freeze of 10 s or more, or an error
  exit, the launcher offers a ZIP of the logs, settings and basic PC details
  (user name, profile folder and PC name removed) and a pre-filled GitHub
  issue. Nothing is sent automatically. `PT5_CRASH_REPORT=0` turns it off.
- `[io-unmapped]` log lines name textures the game binds from memory with
  no IO mapping (diagnostic only; none at High Speed Ring).

### Changed
- **Profile-guided builds.** The player `gt5.exe` is optimised with a
  profile recorded from a High Speed Ring race: about +6% FPS there, and
  late frames 24% -> 13% in the measurement.
- **Race starts and texture streaming** (Prologue renderer ports): a fast
  pipeline lookup, textures placed in 64 MB heaps, a 64 MB upload ring and
  LRU texture eviction. Race start 182 -> about 85 ms, starting grid
  246 -> 125-148 ms; same FPS and memory. Switches: `RSX_PSO_FAST_LOOKUP=0`,
  `RSX_TEX_PLACED=0`, `RSX_TEX_RING_MB=0`, `RSX_TEX_LRU=0`.
- **Cold-cache stutter.** Large textures decode in parallel row bands
  (grid frame decode 21 -> 11 ms), the button-prompt replacer hashes only
  the texture sizes it can replace (42 -> 0 ms on the first grid frame), and
  fewer descriptors are rebuilt per draw. Switches:
  `RSX_TEX_DECODE_THREADS=0`, `RSX_TEX_REPL_SIZES=0`.
- **Arcade car list about 10% faster.** Outside races, the render lanes the
  car list waits on are pinned to their own cores; races are unchanged.
  `SPURS_LANE_PIN_SCOPE=0` restores the fixed set.
- **SPU DMA fast paths**: about +2% at High Speed Ring and up to +5% at Eiger
  Nordwand in rain (single runs). `SPU_DMA_FAST=0` restores the old path.
- Unused SPU function names are no longer stored in `gt5.exe` (they matched
  a public antivirus rule).

### Fixed
- **No menu music or race sound.** Two arithmetic differences between the
  PS3's SPU and x86 corrupted the audio DSP's output: `0 x` an extended-range
  estimate gave NaN instead of 0, and rounding made one 1 Hz filter
  unstable. Both now follow the SPU. Corrupt audio blocks are also contained
  so one bad source cannot silence the whole mix (`AUDIO_NONFINITE_SCRUB=0`,
  `GT5P_AUDIO_ROUND_ZERO=0` for diagnosis).

### Upgrading
- Existing setups are kept: replace the player folder and launch
  `tourline5.exe`. Storage, saves, settings and the shader cache stay where
  they are, and no new setup is needed.

## [0.1.3] - 2026-11-12

Antivirus hotfix for 0.1.2: no gameplay changes. Built from the 0.1.2 tag
plus one change, with the same clean release pipeline.

### Fixed
- Bitdefender and engines using it (ALYac, Arcabit, Emsisoft, GData, VIPRE)
  flagged `gt5.exe` as `Gen:Variant.Lazy`. Byte bisection on VirusTotal traced
  the static score to a run of about 25 `getenv`/`_putenv_s` default-setting
  pairs at the top of `main`; they are now one table and loop with the same
  variables, values and precedence. The fixed package scanned clean.

## [0.1.2] - 2026-11-12

Fixes the intermittent race freeze and Eiger Nordwand weather-change crash,
and adds RecompLauncher Easy Setup. Built from main with the same clean
release pipeline as 0.1.1. The player ZIP contains no game files; you need
your own BCUS98114 disc and matching 01.04 installed data/executable.

### Added
- RecompLauncher Easy Setup: named disc/update inputs and an optional decrypted
  executable or RPCS3 input, with verified import through the native launcher.
  Missing or invalid inputs fail without opening another setup dialog. Direct
  launches retain the built-in setup dialogs.
- A RecompLauncher manifest (`launcher/recomplauncher.json`, 16 GT5 settings,
  storage and log buttons) and relocated-storage following; see
  [docs/recomplauncher.md](docs/recomplauncher.md). RecompLauncher's own UI is
  not yet checked.
- Preload known shaders > **Before start** (GT5 Phase 44; F1, launcher,
  `shader_warmup_before_start`, `RSX_SHADER_WARMUP_STARTUP=1`): the learned
  menu and race pipelines are prepared before the game starts, on every CPU
  core but one (`RSX_SHADER_WARMUP_THREADS`), with a "Preparing shaders
  N / M" badge. In background stays the default.
- GPU driver stamp (GT5 Phase 44): `gpu-stamp.txt` in the shader cache
  records the GPU and driver. After a driver update, saved driver pipelines
  and the race pack from the old driver are rebuilt without being read, and
  the learned sets are prepared before start once
  (`RSX_PSO_GPU_STAMP=0`, `RSX_PSO_GPU_STAMP_PRELOAD=0` turn these off).

### Changed
- GT5's SPU job lanes start together (GT5 Phase 44): the Arcade car list draws
  about 9% more frames; races are unchanged. `SPURS_LANE_STAGGER_MS=1` restores
  the 1 ms per lane stagger.
- `GT5P_SPU_FMA` (opt-in) now covers four job bodies: `1` selects all of them,
  or name bodies (`GT5P_SPU_FMA=b0196FF40` is the previous `1`). No gain
  measured at Eiger Nordwand (GT5 Phase 45).
- Eiger Nordwand in rain runs about 8% faster (GT5 Phase 45): its frame waits
  on one SPU loop, now compiled as a single function with the SPU registers
  in host vector registers. New required post-SPU-lift step
  `scripts/spu_region.py`; `GT5P_SPU_REGION=0` restores the old code.

### Fixed
- Crash after loading Eiger Nordwand with weather change (GT5 Phase 43): job-body
  functions reached only through run-time tables are now lifted, instead of
  falling to the SPU interpreter until a lane's stack overflowed. Requires
  a re-lift (`scripts/lift_spu_jobs.py`; `SPU_GAP_SEEDS=0` without).
- The intermittent 1-4 s race freeze (GT5 Phases 32 and 42): SPU lock-line
  waiters starved an idle-priority game thread holding the lock. Waiters now
  sleep after 1 ms (`SPU_LOCKLINE_SLEEP_US=0` restores the old spin).

## [0.1.1] - 2026-11-11

Adds the native setup launcher, 21:9 ultrawide and GT5-specific CPU work to
0.1.0. The player ZIP, `project-tourline-5-v0.1.1-windows-x64.zip`, still
contains no game files: `tourline5.exe` asks for your own GT5 disc dump,
01.04 installed data and decrypted update executable. Pre-alpha. The full
record is [docs/progress-gt5.md](docs/progress-gt5.md), Phases 37 to 41.

### Added
- Native `tourline5.exe` setup: Prologue-style storage selection, GT5 disc and
  01.04 installed-data pickers, verified executable import and temporary-copy
  RPCS3 decryption. Saves/settings/cache use remembered default or custom storage.
- Change storage/game files and Open storage shortcuts, verified migration with
  the source retained, managed F1 restarts, and launcher-aware player packaging.
  [Setup instructions](docs/setup.md).
- Ultrawide 21:9 for GT5 (F1 VIDEO > Aspect ratio, `video.aspect`): the race
  and scene cameras widen with the same vertical view, and menus and the HUD
  stay centred at 16:9. The aspect constants are measured for v104 (GT5 Phase 41).
- GT5 screens for the settings page: skip opening movie, F1 pauses a race,
  the Options "PC settings" tag and controller-style button prompts (GT5 Phase 38).

### Changed
- GT5 audio now uses its own CPU-dispatched FMA mixer/decoder/output images
  on supported CPUs, with portable fallback (`GT5P_AUDIO_NATIVE=0`).
- Verified SPU-progress wait loops can yield, and native table clears use
  bulk writes. A fingerprint-checked FMA render-body copy is available for
  testing with `GT5P_SPU_FMA=1`, off by default. See GT5 Phase 40 for coverage.

### Fixed
- Disc assets in setups with separate writable storage: absolute disc mappings
  now bypass the save-data root, allowing GT5 to open `GT.VOL` and read artwork
  not present in the installed update.

## [0.1.0] - 2026-11-11

The first tagged release of **Project Tourline 5**, the Gran Turismo 5
(BCUS98114, 01.04 update executable `BCUS98114-v104`) target of this port.
A game-file-free player ZIP, `project-tourline-5-v0.1.0-windows-x64.zip`,
is attached to the release: the engine, its runtime DLLs, glyph art and
licences, with setup notes (`START-HERE.txt`). It contains no game files;
you supply your own disc and the decrypted 01.04 executable. Or build it
with [Building from source](README.md#building-from-source). Pre-alpha: GT5 boots to the main menu,
plays its movies and sound, and Arcade races load and drive; known faults,
including the intermittent race freeze (GT5 Phase 32), are open. The full
record is [docs/progress-gt5.md](docs/progress-gt5.md).

### Added
- Ports from Tourline Prologue v0.2.5, adapted to GT5 (GT5 Phase 33; review in
  [docs/prologue-sync-review.md](docs/prologue-sync-review.md)): analog
  trigger pressure past the digital threshold, a PPU join-lifetime fix,
  trigger dead zone and response curves, optional SDL vibration and trigger
  feedback (off by default), the 128-byte pad capability fix, cheaper shader
  text, a source-keyed shader bundle, separate demand and preload shader
  badges, above-normal process priority, an optional texture LRU (off) and
  bounded hitch tracing. Each has an environment switch that restores the
  old behaviour.
- DXC (Shader Model 6) guest shader compiler with its own cache directory
  (GT5 Phase 34; [docs/dxc.md](docs/dxc.md)), and **the default**. FXC is
  F1 > Shaders > Shader compiler, or `RSX_SHADER_COMPILER=fxc`.
  `scripts/get_dxc.py` fetches the pinned DLL pair.
- Release tooling: `scripts/release/clean_build.py` builds the player ZIP
  from a fresh clone of the tag with only explicit private inputs (the
  decrypted executable and captured SPU images) and pinned third-party
  downloads; `scripts/release/package_player.py` packages an allowlist and
  refuses game-format files. `get_sdl2.ps1` now checks a SHA-256 pin.
- Race preparation on helper threads and a one-file race pipeline pack,
  `shadercache/warmup-v1/race-pack-v1.bin` (GT5 Phase 35). Switches:
  `RSX_RACE_PREWARM_THREADS=1`, `RSX_RACE_PREWARM_PACK=0`.
- GT5 race scope (GT5 Phase 36). The renderer now recognises GT5's race
  loads from measured screen files (`screen_files`, `scope_labels` in
  `config/targets.json`), so it learns the race pipeline set and prepares it
  while a race loads. Measured live on High Speed Ring: 781 pipelines ready
  in 93 ms from the pack. `GT5P_RACE_SCOPE=0` turns it off. Measured for
  Arcade races only.
- The PT5 logo as the executable and window icon, with version information
  in the executable.
- Isolated runtime setup: `scripts/setup_runtime.py` prepares
  `../ps3recomp-tourline5` (ps3recomp `5244db9` plus
  `patches/ps3recomp-5244db9-local.patch`).

### Changed
- Preload known shaders is on by default for new settings files. An
  existing explicit `shader_warmup = false` stays false.
- Pipelines learned in a race leave the menu preparation set
  (`RSX_PSO_MENU_EXCLUDE_RACE=0` keeps them in both).

### Known limits
- No installer or launcher: the game files are placed by hand
  (START-HERE.txt).
- DXC became the default with synthetic output differences against FXC
  still open (see docs/dxc.md); FXC is the rollback.
- The first launch with DXC compiles every shader afresh: DXC uses its own
  cache folder, and FXC caches are kept but not used.
- GT Life, licence tests, special events and online modes were not measured
  for race scope.
- Native control-assignment and opening-movie options from Prologue need
  GT5 measurements and are not included.

## Earlier work (untagged)

### Disc build (BCUS98158), October 2026: Phases 17 to 72

Summarised here; the evidence is in [docs/progress.md](docs/progress.md).

- A second target, the US disc release. Its per-build constants are in
  `config/targets.json`, and its ps3recomp changes are in `patches/`.
- It boots to the main menu, with the 3D scene, menu music and pads (SDL2,
  XInput, keyboard). The opening movie plays through FFmpeg.
- Dealerships draw, with car materials, shadows and cube maps; the page
  freezes are fixed. Daytona's Race Mode page draws with an animated crew,
  and a race loads and drives. Frame rates run from about 22 to 30 fps.
- A settings file and an in-game settings page (F1), a profiler (F3), and a
  "Compiling shaders" badge. Shaders compile on a worker pool and are
  cached on disk.
- Lifter fixes found on the way, which apply to any title:
  - PPU: `stvlx`, `stvrx`, `lswi`, `stswi` and `vsum4sbs` were lifted as
    no-ops; jump tables behind seeded case starts were dropped.
  - SPU: `bisl $r0,$r0` wrote the link before reading the target;
    indirect branch targets kept their low bits; loop entries reached only
    through an address table were never lifted.
- Test drivers in `scripts/test/` and diagnostics in `scripts/diag/`.

### Changed
- Build against current ps3recomp `master` (`795ac34`) instead of the August
  snapshot branch `wip/gt5p-session-diagnostics`. Every toolkit PR this port
  opened (#97–#112) has since merged. Boot went from a random 7–108 PDIPFS assets
  to **116 in 8 of 8 runs**, and main no longer spins on the 264-byte allocation.
  A/B: the August lift on the new runtime gives 107 in 6 of 6 runs, so the
  runtime removed the nondeterminism and the re-lift (42,316 functions, up from
  40,320) accounts for 107 → 116. Toolkit changes from sibling ports that plausibly
  apply here, none yet shown to be the specific cause: per-thread TLS blocks
  (`cbeae9b`, found by Guitar Hero III), one callee-save snapshot per register in
  the lifter (`135b95a`, GH3), dropped NULL-page stores (`7228188`, GH3).
- `src/gen/ppu_hle_nids.cpp` regenerated from the toolkit under build (`--all`,
  1,094 handlers).
- README restructured to the house order; the phase-by-phase investigation moved
  to `docs/progress.md`.

### Added
- `PS3_HEADLESS=1`: no window, but the vblank/flip clock and FIFO drain still run.
  For RDP and unattended runs.
- `scripts/soak.sh`: N headless boots, unique PDIPFS assets per run.
- `ROADMAP.md`, `CHANGELOG.md`.

### Earlier
Phases 1–15 predate this file; see [docs/progress.md](docs/progress.md). Highlights:
NPDRM SELF decryption from a RAP, the `--hle-stubs` import path, SPU job capture
and lift, jump-table recovery (`scripts/jumptables.py`), and the `longjmp`
fix (`scripts/setjmp_patch.py`).
