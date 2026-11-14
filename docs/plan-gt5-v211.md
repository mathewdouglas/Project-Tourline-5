# Plan: the GT5 2.11 update executable, and master mod

Status: plan only (2026-11-14, branch `feat/gt5-v211` from tag `v0.1.5`).
Nothing below has been lifted, built or run. Statements are marked
**measured** (checked on this machine for this plan), **from source** (read
from a public page, cited) or **hypothesis** (not verified).

The current target is `BCUS98114-v104`, the 01.04 executable that the disc's
`INSDIR/DATA000.PKG` installs. This plan adds a second target,
`BCUS98114-v211`, for the 02.11 executable, and then loading the "GT5 Master
Mod" on top of it.

## 0. Corrections to the brief

- **2.11 is not GT5's final update** (from source). Polyphony's notices run on
  to 2.17 (July 2014), which changed DLC reinstallation after the servers
  closed with 2.16. The mod author's LAN guide calls 2.11 the last update
  with content ("There is no additional content added past 2.11"), says to
  install "21 of 27 updates" to reach it, and offers a downgrade for consoles
  above 2.11. So 2.11 is the right target for the mod, but "latest update"
  tooling (RPCS3's update check) will overshoot it.
- **The mod is data, not an executable patch** (from source, partly). The
  author's guide installs it by copying a `PDIPFS` folder over
  `/dev_hdd0/game/<TITLE_ID>/USRDIR` and overwriting files; it mentions no
  EBOOT, GT.VOL or RPCS3 patch. The mod's own readme and the GTPlanet thread
  could not be read (HTTP 403), so "data only" is still a hypothesis to check
  against the actual archive.

## 1. Inputs

### What is on this machine (measured)

| Item | Where | State |
| --- | --- | --- |
| US disc dump, BCUS98114 disc 02.00 | `AGENTS.local.md` path | `PARAM.SFO` `VERSION` 02.00, `APP_VER` 01.00; `INSDIR/DATA000.PKG` 201,251,408 bytes, `INSDIR/PARAM.SFO` `APP_VER` 01.04 |
| RPCS3 `dev_hdd0/game/BCUS98114` | RPCS3 folder in `AGENTS.local.md` | `APP_VER` **01.04**, `PS3_SYSTEM_VER` 03.5000; `EBOOT.BIN` 26,708,368 bytes plus a decrypted `EBOOT.elf` 26,705,632 bytes (= `pt5-player.json` size); 16,450 files under `USRDIR/PDIPFS` (package plus the in-game install), `db/`, `grim2/`, `installed00.dat` |
| RPCS3 `dev_hdd0/game/BCES00569` | same | **EU GT5, `APP_VER` 02.07**, `PS3_SYSTEM_VER` 04.1000. `EBOOT.BIN` 9,486,240 bytes (an SCE container; compressed, not decrypted). 11,784 `PDIPFS` files, 2.9 GB, no `installed00.dat` or `db/` |
| RPCS3 `dev_hdd0/game/NPEA00502` | same | GT6 demo, `APP_VER` 01.05; unrelated |
| GT5 update PKGs (any version) | searched `Downloads`, `Desktop`, `D:\Games` to depth 5 | **none found** |
| Master mod archive | same search | **none found** |
| 2.11 decrypted executable | | **none** |

Two things follow from the EU 02.07 install, though it is another region:

- Later updates ship far more data than 01.04 did: 11,784 package files in
  2.9 GB against 1,330-1,335 files in 201 MB for 01.04. So 2.11 changes the
  installed `PDIPFS` data substantially, not only the executable.
- The 02.07 EBOOT is a compressed SELF of about 24.9 MB once inflated (its
  header's data size, `0x017B4D90`), close to 01.04's 26.7 MB. Same order of
  size; nothing more can be read without decrypting it.

The 01.04 RPCS3 install is the reference for every v104 measurement in
`docs/progress-gt5.md`. **Do not upgrade it in place**: install 2.11 into a
second RPCS3 folder (a portable copy; the launcher's own downloaded RPCS3
under `<storage>\tools` is another option).

### What the user must provide

1. **The US update packages from 01.05 up to and including 02.11**, as `.pkg`
   files. The author's guide says reaching 2.11 is 21 installs (from source);
   whether each package carries only its own `PDIPFS` delta, so that all
   of them are needed in order, is a hypothesis to measure by installing
   them one at a time and diffing the file lists. They are free Sony
   downloads; this project will not fetch them, and RPCS3's built-in update
   check installs up to the latest (2.17), past the target.
2. **The decrypted 02.11 `EBOOT.elf`**, made with RPCS3's *Utilities >
   Decrypt PS3 Binaries* on the installed `EBOOT.BIN` (the same route as
   01.04, Phase 6). Or permission for a later session to run
   `rpcs3.exe --decrypt` once the game slot is free.
3. **The master mod archive** for the US region and 2.11 (the guide says to
   pick "the latest version and region corresponding to your game"; search
   titles show "[1.9.2] GT5 Master Mod", unconfirmed).
4. Optional: DLC. The guide recommends DLC "unlocked" through the mod's debug
   menu rather than requiring purchased packs. Whether DLC content files
   (and their `.edat` licences, which need `.rap` keys) are needed at all is
   open; keys never go into this repository either way.

### Identity to pin

`config/pt5-player.json` pins one executable (`elf_sha256`, `elf_bytes`) and
the disc package. 2.11 needs its own entry: the 02.11 ELF hash and size, the
expected `APP_VER` (hypothesis: `02.11`), and the installed data's file count
and bytes after all updates. If update packages are accepted as setup inputs,
also each package's name, size and SHA-256, in install order. All of these
are measured from the user's files in milestone M0; none are known now.

## 2. Inventory of per-executable values

Every value below was measured for v104 and **must not be carried over**.
"Script" means a repo script derives it from the ELF; "measure" means a run
of the game is required; "hand" means static analysis by a person
(`cdis.py`, `xref.py`, `vtab.py`, Ghidra/REA).

A proposed helper, `scripts/port_addresses.py` (task T5), would speed up the
"hand" rows: for each v104 address, take its containing function, mask the
relocation-dependent fields (branch targets, TOC displacements, `lis`/`addi`
pairs), find the function with the same masked bytes in 2.11, and report the
corresponding offset with a confidence. It proposes values; every proposal is
then confirmed by the method in the table.

### Lift inputs (`config/targets.json`, read by CMake and `scripts/target.py`)

| Key (v104 value) | Purpose | 2.11 method | Risk |
| --- | --- | --- | --- |
| `code_end` (`0x011752A4`) | lift boundary | script: end of the last executable section | low |
| `seeds` (`extra_seeds_bcus98114_v104.json`, 4,978 lines) | function starts | new file `extra_seeds_bcus98114_v211.json`, built by `jumptables.py --write` and the required pre-lift `code_labels.py --write`; then any seeds added by hand after runtime `no lifted entry` lines, found again by running | medium: hand seeds only show up at run time |
| `setjmp` / `longjmp` (`0x00D49E94` / `0x00D49FB8`) | `setjmp_patch.py`, `gt5p_longjmp.cpp` | script: `find_setjmp.py` (instruction shape) | low |
| HLE stubs (`analysis/hle_stubs.json`) | import thunks | script: `gen_hle_stubs.py`; diff the `_named.json` against v104's | low to make; new NIDs are section 3 |
| `wws_job_pm_ea` / `_size` / `_extra_funcs` (`0x0182C400`, 12,032, `0xA2C`) | WWS policy module | script/hand: exact byte search for v104's module (01.00 to 01.04 was byte-identical) | low |
| `wws_job_bodies` (36 entries) | SPU job bodies | script: `diag/find_bodies.py` (`0xC0DEC0DE` headers, 16-aligned); compare by bytes with v104's 36 | low to find; medium if bodies changed (section 3) |
| `analysis/spu/dump`, `taskdump` | captured job images and task ELFs | fingerprints are content hashes, so v104's captures still register when unchanged; new images found by `SPU_DUMP_MISS` runs | medium: needs routes through every screen |
| `spu_registry` (`src/gen/spu_workloads_bcus98114_v104.c`) | tracked registry | generated by `lift_spu_jobs.py` as `..._v211.c` | low |
| `boot_from` `bdvd`, `hdd_title` false, `gcm_ctrl_base` `0x03000000`, `spurs_synth_job_done` false | boot settings | keep, then confirm at the first boots | low |
| `spu_sn_defer` 4 | SPU notify deferral (Phase 19) | keep; re-measure on the car list | low |

### Post-lift steps and their keys

| Step / key (v104) | 2.11 method | Risk |
| --- | --- | --- |
| `setjmp_patch.py` | runs from the new `setjmp`/`longjmp` | low |
| `spin_patch.py`, `spin_waits` (5 ranges: `0x00D1B930`-`0x00D1B9B0`, `0x00D1DE20`-`0x00D1DE48`, three single sites at `0x00C34BDC`, `0x00C35254`, `0x00C357FC`) | `port_addresses.py`, then confirm each is still a pure wait loop (Phase 40 rule: no stores, no service calls in the waiting path); `spin_waits.py` test must be generalised | medium |
| `ui_mask_patch.py`, `ui_label_mask_func` | Prologue only; GT5 has none. Reports "nothing" | none |
| `spu_native_patch.py`: `spu_native_hooks` (`b0196FF40` `0x4C14` `gt5p_spu_clear_words`), `spu_audio_round_zero_calls` (`j9804ABA8` `0x4E8`) | bodies are named by load address: if the body bytes are identical, rename to its 2.11 address and keep the offset; if not, re-find the loop and re-run `spu_clear.py` and `audio_coefficients.py` | low if identical, high if not (wrong hook = silent corruption) |
| `spu_regpromo.py`, `spu_regpromo` | off by default (measured slower); leave the key out | none |
| `spu_region.py`, `spu_regions` (two loop sets in `b01697100`) | same as native hooks; exactness re-checked with `spu_loop_bench.py` on new captures | medium |
| `spu_fma_images` (four bodies: fingerprint, image id, EA, span) | fingerprints are content; image ids are registry order and EAs move, so regenerate from the new registry; CMake refuses a fingerprint mismatch | low |
| `audio_native_images` (`jD58063CD`, `jEB79FDFC`, `jF523F26F`, `jFDFFAAC1`), `audio_extended_zero_images` | content-named job images; check they are still the audio DSP images (`audio_gt5.py`, `audio_extended.py`) | low if unchanged |
| `return_gap_seeds` (Phase 43, in the SPU lift) | automatic | low |

### Runtime addresses and measured game data

| Key (v104) | Feature | 2.11 method | Risk |
| --- | --- | --- | --- |
| `aspect_16_9_float` `0x019133B0`, `ui_aspect_16_9_float` `0x01914A48`, `scene_camera_aspect_16_9_float` `0x018EB70C` | 21:9 (Phase 41) | hand, by Phase 41's recipe: all `0x3FE38E39` floats, their `lfs d(r2)` readers via the OPD TOCs, then `GT5P_MEMSNAP` at the menu | medium; wrong value is visible, not silent |
| `menu_listbox_vtable` `0x01870B78`, `menu_listbox_range` `0x40000000-0x50000000` | F1 race pause | `rtti.py`/`vtab.py` for `mListBox`; range by memory scan in a race | low |
| `top_menu_word` `0x0199359C=2` | top-menu detection | measure: `GT5P_MEMSNAP` + `memsnap_diff.py` at top menu vs Options (Phase 38) | medium: data layout moves |
| `screen_files` (`installed00.dat`, `OpeningMovie=MOVIE/tv_451_120285132.pam`, `Arcade=9/HV/GE`, ...) and `$GT5MilestoneFiles` in `scripts/test/milestone.ps1` (adds `GTLife=9/UC/F0`) | race scope, test routes, skip movie | measure: `PAD_FILE_LOG=2` along the Arcade route. Hypothesis: `PDIPFS` names are hashes of logical paths, so unchanged files keep their names, but updated screens may read a different first file | medium; every automated test depends on it |
| `screen_roles`, `scope_labels`, `movie_screen` | derived labels | follow `screen_files` | low |
| `frame_pacer_sleep_lr` (unset for GT5) | pacer skip | Phase 40 found no pacing sleep in v104; re-check in one HSR trace | low |
| `assets/glyphs/textures.txt` GT5 section (content hashes) | controller prompts | `RSX_TEX_REPL_DUMP` on a race visit; content hashes survive only if the update did not replace the textures | low |
| `src/main.cpp` `apply_env_defaults()` keyed on `GT5P_TITLE_ID == "BCUS98114"` | all GT5 runtime defaults | applies to v211 automatically (same title id). `PS3_PPU_PRIO_LOW_MAX=2240` is the frame thread's guest priority: re-check | low |
| Runtime patch: `SPURS_LANE_PIN_MENU` default `4`, `_RACE` `0,1` | car-list lane pinning (Phase 46) | workload ids follow creation order; confirm with the renderer's scope report and lane stats | medium |
| Env knobs naming bodies (`GT5P_SPU_FMA=b0196FF40`, `GT5P_SAMPLE_WID`, docs) | diagnostics | rename in docs per target | low |
| `GT5P_ARGV_BASE` `0x00F00000` in `src/main.cpp` | argv block | **measured:** in v104 it lies inside the first `PT_LOAD` segment (`0x10000`-`0x182F668`, below `code_end`), and `setup_argv` zeroes 64 KB there after the ELF is loaded. It lands on code bytes in v104, which lifted code never reads; check where it lands in 2.11 and consider moving it out of the image for both | low now; latent |
| `src/main.cpp` crash dump bound `0x00E40000` | diagnostics only | Prologue's code end; make it `GT5P_CODE_HI` | cosmetic |

### Player, release and test files

| File | v104-specific content | Change |
| --- | --- | --- |
| `config/pt5-player.json` | one target's ELF hash/size, disc package | per-target entries (section 5) |
| `launcher/launcher.cpp` | `APP_VER` "01.04" checks, `target=BCUS98114-v104`, messages | target-driven (section 5) |
| `scripts/stage_player.cmake`, `scripts/release/clean_build.py`, `package_player.py` | `BCUS98114-v104`, seeds file name | `--target` parameter |
| `scripts/test/spin_waits.py`, `audio_coefficients.py`, `audio_native.py` | assert or read `BCUS98114-v104` | read the selected target |
| `scripts/test/gt5_reset.py`, `cellfs_paths.py` | `BCUS98114` paths | unchanged (same title id) |
| `shadercache/warmup-v1/` (`menu-v1.txt`, `race-v1.txt`, `race-pack-v1.bin`, `fp-lookup-gt5-v1-<tag>.bin`), `scratch/pgo/` | learned per executable/data | separate per target (section 5) |

## 3. Runtime and import differences

**To expect (hypotheses).** From 01.00 to 01.04 the import list changed by
on-screen-keyboard and NP additions, one `cellUsbd` call and a dropped
`cellImeJp` (Phase 6). The 2.x updates added DLC purchase and installation
(2.00 onwards), more online features and, per Polyphony's notices, a new DLC
install path in 2.17. Likely new or more-used imports: `sceNpDrm*`
(DLC licences; the runtime has `edat.h` but no `sceNpDrm` library was found
by name), `sceNpCommerce2`, `cellGame` content-permit calls for DLC, and NP
matching/signalling for the lounge. The servers closed in 2014, so every NP
service must answer "offline" cleanly rather than be implemented.

**How to measure (M1, static, no run):**

1. `gen_hle_stubs.py` on both ELFs and diff the `_named.json` module/NID sets.
2. For each new NID, check whether the runtime has a handler
   (`gen_hle_nids.py --all` output). Missing ones are the first boot
   blockers to expect.
3. `find_functions.py` counts and `jumptables.py`/`code_labels.py` counts, as
   Phase 6's table did for 01.00 to 01.04.
4. Byte-compare the policy module and the 36 job bodies; list moved,
   unchanged and new bodies. Phase 6 found 11 of 36 byte-identical across
   01.00 to 01.04, so expect real SPU changes.

**At run time (M3 onwards):** `[spu-smc] microstep ... handing to the
interpreter` names an unlifted body; `[spurs-job] dispatch MISS` names an
uncaptured image; `[ppu] indirect tail to ...: no lifted entry` names a
missing seed; `PS3_HLE_TRACE` lists unimplemented imports. An RPCS3 log of a
2.11 first boot, driven by hand as in Phase 5, is the reference for the file
sequence and the new install steps.

**The in-game install.** v104 runs a first-boot install from `GT.VOL`
(Phases 8-10). With 2.11's much larger package data the install may differ
(more files already present, a new `installed*.dat` marker). The launcher's
"incomplete install finishes in the game" path must be re-checked.

## 4. Master mod

**What it changes (from source, partly hypothesis).** Installation copies the
mod's `PDIPFS` tree over the installed one, overwriting files; the original
`PDIPFS` restores the game. It supports BCES00569, BCUS98114, BCJS30001 and
BCAS20108 and "requires the game to be on version 2.11". It adds a hidden
debug/admin menu (Select on the main menu), DLC and track unlocks, and a LAN
mode played over XLink Kai. The guide targets CFW/HEN consoles; RPCS3 is not
covered in it. Hypothesis: the menu and unlocks live in the game's UI
scripts and data inside `PDIPFS`, and the 2.11 executable is unmodified.

**What the port needs.**

1. **A 2.11 lift** (sections 2 and 6). Without it nothing else applies.
2. **A data overlay rather than overwriting.** Keep the verified clean
   install in storage, and put the mod in `<storage>\mods\<name>\PDIPFS`.
   A runtime switch (proposed `PS3_FS_OVERLAY=<dir>`, off by default) makes
   cellFs look up `/dev_hdd0/game/BCUS98114/USRDIR/PDIPFS/...` in the overlay
   first, for open and stat, with writes still going to the real install.
   Disabling the mod is then a setting, the clean data stays hash-verifiable,
   and the in-game install cannot overwrite mod files. The overlay must win
   over files the in-game install writes later.
3. **If the mod does patch the executable** (to verify): a static
   recompilation cannot apply code patches at run time. A patched executable
   is another target: identify it by hash, lift it as its own target, or
   apply the patch as a pre-lift step to the decrypted ELF. Data-only pokes
   can use the existing `GT5P_POKE_BOOT`.
4. **Effects on measured values.** Screen milestones match path tails, so
   they survive replaced content; glyph texture hashes and learned shader
   sets may not. Run the M6 route with and without the mod.
5. **LAN mode** needs real UDP broadcast through `sysNet.c` and
   `cellNetCtl`, plus XLink Kai on the host side. Out of scope until offline
   play works; listed as an open question.

**Legal and distribution.** Unchanged rules: no game files, update
packages, mod files, keys or `.rap` files are tracked, packaged or
downloaded by this project. The player supplies the mod archive and
updates. The mod states no licence (the guide credits contributors in the
mod's readme); before the launcher names or recognises the mod by hash, ask
the author. `check_no_game_files.py` already refuses `PDIPFS`-shaped files;
extend it to refuse mod archives by name and size if they ever appear in a
working tree.

## 5. Multi-target support

### Bring-up: one worktree per target (no code change)

`input/`, `generated/`, `analysis/` and `build/` hold one target each. For
M1-M6 the simplest arrangement is this worktree as the 2.11 tree: its own
`input/EMAIN.ELF` (2.11), `input/target` = `BCUS98114-v211`, its own
`generated/` and `build\`. The v104 tree is untouched. The isolated runtime
`../ps3recomp-tourline5` is shared; runtime fixes must keep v104 working, so
each one is checked on both (switch-gated, as every behaviour already is).

### Coexistence in one source tree and one release

| Area | Proposal |
| --- | --- |
| Config | `BCUS98114-v211` key in `targets.json` with `title_id` BCUS98114 and `exe_name` `gt5_211`, so both engines can sit side by side |
| Inputs | `input/<target>/EMAIN.ELF`, `generated/<target>/`, `analysis/<target>/` with the current paths as v104's fallback; scripts get them from `scripts/target.py` (several hard-code `input/EMAIN.ELF` today) |
| Builds | `build_v211\` (already ignored by `build_*/`); `GT5P_TARGET` already selects everything else |
| Storage | one storage root, `targets/<target>/` underneath for `input/`, `gamedata/`, `shadercache/`; saves are per target because a 2.11 save cannot go back to 01.04 (hypothesis: the game upgrades saves) |
| Shader caches | per target. Pipeline keys are content hashes, so sharing may work, but learned menu/race sets, the race pack and the fast-lookup table are per executable and data; measure overlap later before sharing |
| PGO | one profile per target, made from that target's lift (`pgo.py` takes `--target`) |
| Launcher | `setup.ini` `target=` already exists; setup asks which version, verifies that target's identity, and starts that target's engine; `pt5-player.json` becomes a map of targets |
| Release | an experimental separate 2.11 ZIP first (each engine is about 176 MB); merge into one ZIP once 2.11 reaches parity. `clean_build.py`/`package_player.py` take `--target` |
| Setup route for 2.11 | first: import an existing RPCS3 2.11 install plus `EBOOT.elf` (the existing import path, with `APP_VER` 02.11). Later: install a folder of the player's update PKGs in order with the launcher's RPCS3, pinned by hash, as the disc route does for `DATA000.PKG` |

### Milestones

| | Milestone | Done when |
| --- | --- | --- |
| M0 | Inputs | 2.11 installed in a second RPCS3, ELF decrypted, identity and data counts recorded |
| M1 | Static triage | Phase 1/6-style table: sections, `code_end`, imports diff, function counts, policy module and bodies compared |
| M2 | Lift builds | all pre/post-lift steps run, `/* TODO:` grep clean, `gt5_211.exe` links |
| M3 | Warning screen | boots to the epilepsy warning |
| M4 | Menu | first-boot install completes, main menu at 30 FPS |
| M5 | Movie and sound | opening movie plays, menu music |
| M6 | Race | Arcade HSR race runs; `gt5boot.ps1 -Route race` works with new milestones |
| M7 | Feature parity | ultrawide, F1 pause, prompts, skip movie, race scope, race pack |
| M8 | Performance parity | SPU tuning keys re-derived, HSR/Eiger/car list within noise of v104, PGO profile |
| M9 | Multi-target product | launcher/release support both, setup route for 2.11 |
| M10 | Master mod | overlay switch, mod's debug menu reachable, Arcade race with mod data |

## 6. Ordered tasks

Sizes: S under half a day, M one or two days, L three days or more. The
01.00 to 01.04 retarget took one session (Phase 6); v104's bring-up from
warning to menu took Phases 7-12, mostly runtime fixes that 2.11 inherits.

| # | Task | Size | Needs |
| --- | --- | --- | --- |
| T1 | User installs updates up to 2.11 into a second RPCS3; decrypt `EBOOT.BIN` | S (user) | update PKGs |
| T2 | Record identity: ELF SHA-256/size, `PARAM.SFO`, `PDIPFS` count/bytes, per-package file lists | S | T1 |
| T3 | Static triage and Phase 6-style comparison table in `docs/progress-gt5.md` | M | T2 |
| T4 | `BCUS98114-v211` key with lift inputs only (`code_end`, `setjmp`, policy module, bodies, seeds) | S | T3 |
| T5 | `scripts/port_addresses.py`: masked-function matching from v104 to 2.11, report only | M | T3 |
| T6 | Lift, post-lift steps, build `gt5_211.exe` in this worktree | M | T4 |
| T7 | RPCS3 reference: first-boot log of 2.11 (driven by hand) | S (user) | T1 |
| T8 | Boot to warning, then menu: new imports, seeds, job images, install differences | L | T6, T7 |
| T9 | Screen milestones (`PAD_FILE_LOG=2`) and test routes for 2.11 | M | T8 |
| T10 | Re-derive runtime addresses: aspect, listbox, top-menu word, glyphs, spin waits | M | T5, T9 |
| T11 | Re-derive SPU tuning keys (native hooks, regions, FMA, audio images) with exactness tests | M | T6, T9 |
| T12 | Performance check against v104 on HSR, Eiger, car list; PGO profile | M | T11 |
| T13 | Multi-target source layout (`input/<target>`, script paths, release `--target`) | M | T6 |
| T14 | Launcher: target choice, per-target storage, 2.11 import route | L | T2, T13 |
| T15 | Runtime `PS3_FS_OVERLAY` and launcher mod folder | M | T8 |
| T16 | Master mod test: inspect the archive first (data only?), then menu, debug menu, race | M | T15, user's archive |
| T17 | Update-PKG install route in the launcher (pinned hashes, in order) | L | T14, T2 |
| T18 | Optional: LAN mode over `sysNet` | L | T16 |

## 7. Open questions for the user

1. Which update packages do you have, and is 2.11 (last content update)
   the intended target rather than 2.17 (the last update)?
2. May 2.11 be installed into a second RPCS3 folder, keeping the 01.04
   install as the reference for v104 work?
3. For the release: a separate experimental 2.11 ZIP first, or wait for
   parity and ship one ZIP with both?
4. Should 2.11 eventually replace v104 as the default target?
5. Which master mod version, and has it been checked whether the archive
   holds anything outside `PDIPFS` (an EBOOT, patches)?
6. DLC: do you own any GT5 DLC, and should the port support DLC content and
   licences at all, or only what the mod's debug menu unlocks?
7. Is LAN mode wanted, or is offline play with the mod enough?
8. Other regions: the mod also supports EU/JP/Asia, and an EU 02.07 install
   is on this machine. Out of scope for now?

## Sources

- Polyphony Digital, Update Notice (2.11): https://gran-turismo.com/us/news/02_0006833.html
- GT5 2.17 coverage: https://onlineracedriver.com/2014/07/23/gran-turismo-5-v2-17-update-patch-released
- Nenkai, GT5 LAN Mode guide (master mod install, versions, regions): https://gist.github.com/Nenkai/f46bdce07c4ee019c57494319b292b30
- GTPlanet "GT5 Master Mod" thread (title seen in search, page not readable): https://www.gtplanet.net/forum/threads/1-9-2-gt5-master-mod.395844/latest
