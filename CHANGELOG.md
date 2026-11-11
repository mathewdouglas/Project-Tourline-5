# Changelog

All notable changes to this port. Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/);
versions follow [SemVer](https://semver.org/).

## [Unreleased]

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
