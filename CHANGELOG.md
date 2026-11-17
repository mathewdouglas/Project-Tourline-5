# Changelog

All notable changes to this port. Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/);
versions follow [SemVer](https://semver.org/).

## [Unreleased]

### Fixed
- Slow launcher starts after the game added files to its installed data:
  the check that links installed files to your disc now considers only files
  written since its last pass, instead of opening all ~16,700 again (67 s on a
  cold disk). A new file that is not on the disc still makes it read
  GT.VOL once. `PT5_VOLMAP_ALL=1` restores the old behaviour.
- Problem reports from GT5 2.11 included 01.04's setup.ini (#28).

## [0.1.10] - 2026-11-17

A game copy check, a startup status window, and fixes for a 2.11 main-menu
hang and a long-session slowdown.

**Your first start of 0.1.10 reads your game disc once** (about 20 GB) to
check it: seconds on an SSD, a few minutes on a hard drive. A progress window
shows it and can be cancelled; later starts skip it.

### Added
- **Startup status window.** If the game window has not appeared 1.5 s after
  you press Play, a small window shows what the launcher is doing (checking
  files, starting the game, waiting for its window) with a moving bar and
  the time so far, and closes as soon as the game window opens. The first
  start after an update was reported to show nothing for about a minute; the
  cause was not reproduced here (antivirus checks of the new files are
  suspected). Every start now writes each step's time to
  `logs/launcher-start.txt`, which problem reports include, so a slow start
  shows where the time went. `PT5_START_STATUS=0` hides the window.
- **Game copy check.** Setup now compares your disc folder with the retail
  GT5 disc (BCUS98114, disc version 02.00): every file the game reads
  (GT.VOL and the MOVIE videos) must be present, the right size and
  byte-identical. Each file is read once (about 20 GB, measured at about 20 s
  here; a hard drive takes a few minutes) and only checked again if it
  changes. A damaged or incomplete copy stops setup with the list of bad files.
  Files extracted from an encrypted disc image (the right sizes, the wrong
  contents: issues #15 and #35, a black screen then exit) are found within a
  second from samples of each file, with advice to use a decrypted image.
  Existing installs are checked once at the next start; a copy with problems
  shows a warning with Play anyway or Quit. "Verify game files.cmd" (or
  `tourline5.exe --verify-disc`) reads every file again. RecompLauncher's
  preparation runs the same check, and its Quit exit code shows a message
  there. Problem reports and the F8 debug export now say whether the copy was
  verified and which mods are active. `PT5_DISC_CHECK=0` turns the check off.

### Fixed
- **GT5 2.11: stuck on the GT logo while loading the main menu** (issue #37).
  A loader thread waited forever on a lock in a request that had already
  finished. Lock ownership is now kept by the port itself
  (`PS3_LWM_HOST_STATE=0` restores the old behaviour).
- **Slowdown in long sessions** (issue #37). After about half an hour a debug
  log reopened and wrote every SPU transfer to the log (14.8 GB in one
  player's session). Log limits now stop counting at their cap.

### Known issues
- GT5 2.11: the picture can stop while the game keeps running, seen on the
  garage reward screen and in a B-Spec race (issues #36 and #38). Not fixed
  yet. The F8 debug export now records what the game was waiting for; if it
  happens to you, please attach one.
- GT5 2.11 may still have other issues, including race audio stutter. Use
  01.04 for the most reliable experience.

## [0.1.9] - 2026-11-16

### Added
- **Carry your 01.04 progress into GT5 2.11.** The first time 2.11 starts
  without a save of its own, the launcher offers to copy your 01.04 save into
  it. Your 01.04 save is only read, and from then on the two versions
  progress separately. To copy it later, run "Copy 01.04 save to 2.11.cmd"
  (or `tourline5.exe --copy-104-save`); 2.11's current save is moved to the
  backups folder first. `PT5_SAVE_COPY=0` never offers it. Do not copy a
  2.11 save back to 01.04.
- **Surround sound** (issue #21). GT5 mixes 5.1, which the port used to fold
  down to stereo. F1 > AUDIO > Sound output (or RecompLauncher's Sound
  Output, `audio.output`) now offers Stereo (the default), 5.1, 7.1 and Auto
  (follows the Windows speaker setup). 7.1 adds silent side speakers: GT5
  writes none. A device that cannot play the layout falls back to stereo.
  Restart to apply. `AUDIO_OUTPUT_CHANNELS=2|6|8|auto`.
- **GT5 2.11: 21:9 ultrawide.** Aspect ratio 21:9 now works on 2.11 as on
  01.04: a wider view in races, menus centred. Checked at 1080p
  (2560x1080), 1440p (3440x1440) and 4K (5120x2160), and 1440p at 16:9, in
  the car list, a race and GT Life.
- **GT5 2.11: the remaining 01.04 PC features.** F1 in a race pauses it,
  Skip opening movie works, and Options shows the "PC settings" tag (Select
  opens the page). Controller-style button prompts already worked on 2.11
  (the game's button art is unchanged); now checked.
- **Mods, starting with GT5 Master Mod (2.11).** F1 > Game > Mods (or
  `tourline5.exe --mods`) installs a mod from its ZIP file and removes
  installed ones; F1 > Game > Mod picks which loads (Off, a mod, or a mod
  with one of its add-ons, such as Master Mod's debug camera), then Restart.
  Mods are read over your installed game data, which is never changed, so
  Off is always the unmodded game. Only the mod's game data is kept from the
  ZIP. A mod made for another version or region is refused. Turning a mod
  on copies your save to the backups folder first. Each game version has its
  own mods. Checked with Master Mod 2.0.0a (US): its menu banner, the Select
  debug menu and an Arcade race. `PT5_MODS=0` turns mod support off.

### Changed
- **F1 settings: every page is named on the bar under the title** (Video,
  Shaders, Input, Controls, Feedback, Keyboard, Game, Audio, Performance,
  Debug), the current one highlighted, so you can see where L1/R1 go. Click a
  name to jump to that page. `GT5P_SETTINGS_TABS=0` shows only the current
  page's name, as before.

### Fixed
- **Starting the game sometimes showed nothing for 20 s or more.** Before
  each start the launcher checks whether the game has installed new data (to
  shrink it, as the virtual install does), and it opened all ~16,700 installed
  files to do so: 67 s when Windows had not read them recently, 1.3 s when it
  had. It now reads only the folder listing (0.05 s). It also checked the
  same engine files several times per start (about 1.4 GB read); each is now
  read once. `PT5_VOLMAP_SIG=1` and `PT5_HASH_MEMO=0` restore the old checks.
- **Crash while finishing an early startup exit** (issue #23). The engine
  keeps guest memory mapped until process termination, so background rendering
  cannot read it after it has been released. This fixes the shutdown crash;
  the reason the affected startup exits early is still under investigation.
- **Black headlights on the Ferrari California and other cars.** Glass
  materials now read a current copy of the scene and use the game's pixel
  coordinate origin, so the bulbs remain visible through their covers.
- **21:9: GT Mode's "Information" messages are no longer cut off mid-line**
  (the first My Home and Dealerships messages, and other text boxes that
  clip their text). GT5 placed the text's clip edge for a 16:9 screen, a
  quarter of the screen too far left. `GT5P_UI_MASK_FIX=0` restores it.
  Applies to both 01.04 and 2.11.

### Known issues
- GT5 2.11 may still have issues, including race audio stutter. A packaged
  High Speed Ring sample had hundreds of audio underruns and silent
  stretches; the 01.04 sample had 2 underruns and none silent. Use 01.04
  for the most reliable experience.

## [0.1.8] - 2026-11-16

Fixes for the Windows 10 graphics error, 01.04 race sound and a shutdown
hang. GT5 2.11 is still experimental (may have issues; GT5 master mod support
is still to come).

### Fixed
- **Windows 10: the game no longer stops with a graphics error when a race
  loads or in the dealership** (issues #16, #18, #19; "frame command list
  Close hr=0x80070057"). A 1x1 cube texture's faces were staged 256 bytes
  apart, but texture copies must start on 512-byte boundaries. Windows 11's
  graphics runtime accepted the copy; Windows 10's rejected the frame. Each
  face now starts on a 512-byte boundary (`RSX_TEX_CUBE_ALIGN=0`: as before).
- **Race sound no longer stutters** (01.04, since 0.1.6). The arithmetic
  added for 2.11's race audio made 01.04's audio jobs about 75% slower, so
  the sound fell behind in races: about 17 dropouts a second and a third of
  the audio blocks silent. 01.04 builds without it again (0.1.5's audio
  speed); 2.11 keeps it (`audio_extended_range` in `config/targets.json`).
- **A hang when the game shuts a worker down** (issues #12, #15). Destroying
  an event queue now wakes the threads waiting on it, as on the console, so
  the game's join of that worker returns. Why the game was shutting down at
  that point in those reports is still unknown.

### Added
- `RSX_CMD_CHECK=1` (diagnostic): logs graphics commands that an older
  Windows graphics runtime rejects (copy alignment and bounds, viewports,
  barriers, root bindings). See `docs/renderer-errors.md`.

### Known issues
- GT5 2.11: race sound still stutters. 2.11 keeps the arithmetic that stops
  its race audio going silent, and that makes its sound fall behind in
  races. 01.04 is not affected.
- The Windows 10 fix was confirmed against a reporter's log, not yet on a
  Windows 10 PC.

## [0.1.7] - 2026-11-16

Fixes a crash in the F1 settings page. Otherwise the same as 0.1.6,
including the experimental GT5 2.11 engine (may have issues; GT5 master mod
support is still to come).

### Fixed
- **The game no longer crashes when you open the DEBUG page of the F1
  settings** (issue #17). Pages wrap round, so pressing L1 (Q, Page Up) on
  the first page, or R1 (W, Page Down) to the last one, reached DEBUG and
  closed the game with an access violation. This was a 0.1.6 regression.

## [0.1.6] - 2026-11-16

**Experimental GT5 2.11 support**, a much smaller first-boot install, and
clearer handling of graphics failures. The player ZIP contains no game files;
you need your own extracted BCUS98114 disc (the folder containing
`PS3_GAME`).

### Highlights
- **GT5 2.11 (experimental, may have issues).** The ZIP now carries a second
  engine for the 2.11 update next to 01.04. It is optional: 01.04 stays the
  default and the recommended version. See "GT5 2.11" below.
- **The first-boot install takes about 240 MB instead of 6.5 GB**, and is
  faster. Existing installs are shrunk the same way, once.
- **Graphics failures now stop cleanly** with a message, and the launcher
  can save a problem report and restart the game.

### GT5 2.11 (experimental)
- **This is an early, experimental engine and may have issues.** It boots,
  reaches the menus and runs Arcade races at 60 FPS at High Speed Ring, but
  it has had far less testing than 01.04. Please report problems.
- **Not on 2.11 yet:** F1 race pause, ultrawide, controller-style button
  prompts, Skip opening movie and the Options "PC settings" tag. These need
  measurements for the 2.11 executable and will follow.
- **Master mod support is coming.** 2.11 is the base the GT5 master mod
  builds on; mod support is planned for a later release and is not included
  yet.
- **Setup:** in RecompLauncher add a folder with all 21 official update
  packages (01.05 through 02.11), or import an existing complete 2.11
  installation. Nothing is downloaded. In game, F1 > Game > Set up 2.11
  starts setup, and Game version + Restart switches between 01.04 and 2.11.
  Each version keeps its own game data, saves and shaders; PC settings are
  shared. Other updates (including 2.17) are refused. See
  `docs/setup-v211.md`.
- Fixed during bring-up: silent race audio (non-finite samples), a race
  freeze after about 90 s (guest threads shared one TLS block), a menu
  stall, and a slow Arcade car list (15 -> 32-35 FPS).

### Added
- **The first-boot install takes about 240 MB instead of 6.5 GB.** Files the
  game copies unchanged from the disc's `GT.VOL` are stored as small links
  to the disc, written while the game's own install runs. All installed files
  read back byte for byte as before. The install is also faster: 295-342 s
  instead of 427 s in tests. `PS3_FS_VOLMAP_INSTALL=0` stores full copies.
- **No install prompt.** A first boot answers the game's "Data Installation
  Recommendation" with its default, Yes, and closes "Installation complete".
  `GT5P_AUTO_INSTALL=0` leaves both to the player.
- **Existing installs shrink too.** Before play, the launcher converts an
  install's verified copies of `GT.VOL` into links, once. This applies to
  installs imported from RPCS3 and installs made by earlier versions.
  `PT5_VOLMAP=0` turns it off.
- **Graphics failure handling.** A failed frame submission, failed Present
  or lost graphics device now stops the game with a clear message instead of
  a frozen picture, and logs what failed. From the launcher you can save a
  problem report, open a pre-filled GitHub issue, or save the report and
  restart. Nothing is sent automatically.
- Modern control options connect to GT5's own button assignments.

### Changed
- Created guest threads get their own thread-local storage from the game's
  template (`PS3_TLS_FROM_CRT=0` restores the shared block). This was the
  2.11 race freeze; 01.04 uses the same runtime and was re-checked (High
  Speed Ring, rainy Eiger, car list, boots).
- SPU lanes can rejoin a running render pass (`SPURS_LANE_JOIN=0` off).

### Known issues
- GT5 2.11: see above. A rare startup stall after the install check (the
  boot movie never finishing) was seen 4 times in 76 early boots and not in
  95 boots of this build; its cause is unknown.
- Small white specks on some car edges, faint on 01.04 and stronger on 2.11.
- The launcher's conversion of an existing install has not yet been run on
  real player storage. The same method, run by `scripts/volmap.py`, has
  been run on real data, and a race played on the converted data.
- To undo the links, use `scripts/volmap.py restore`. The launcher has no
  option for this yet.
- Course Maker at 21:9: the map is misplaced and right-hand labels are
  clipped.

## [0.1.5] - 2026-11-14

Smoother first visits, fewer hitches when other programs are busy, and
faster SPU work. Built with the same clean, profile-guided release pipeline.
The player ZIP contains no game files; you need your own extracted BCUS98114
disc (the folder containing `PS3_GAME`).

### Highlights
- **Faster:** about +4% FPS at High Speed Ring and +6% at rainy Eiger from
  faster SPU integer helpers.
- **Fewer hitches:** new places build their pipelines on several threads,
  shader building gives way to the game in races, and the game's frame
  thread no longer drops to idle priority.
- **Shorter pauses on first loads**, and no frame-rate dip in the Arcade car
  list when a car appears.
- **Course Maker's map panel no longer flickers.**

### Known issues
- Course Maker at 21:9: the map is misplaced and right-hand labels are
  clipped.

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
