# Plan: the GT5 2.11 update executable, and master mod

Status: bring-up and player setup in progress (2026-11-15). The 2.11 engine
reaches the animated main menu. Optional package setup, dual-version imports,
separate game/save/cache roots, unified packaging and F1 version controls are
implemented; see [2.11 setup](setup-v211.md). Race/parity and a clean release
build remain acceptance work. The inventory below retains the original plan's
measurements and hypotheses.

The current target is `BCUS98114-v104`, the 01.04 executable that the disc's
`INSDIR/DATA000.PKG` installs. This plan adds a second target,
`BCUS98114-v211`, for the 02.11 executable, and then loading the "GT5 Master
Mod" on top of it.

**Decisions by the user (2026-11-14)**, which settle three of the original
open questions:

1. **The target is 2.11 exactly.** Not 2.17 or any other version. Setup
   refuses any other `APP_VER` with a message that names 02.11.
2. **There is one launcher and one player ZIP.** There is no separate 2.11
   package. `tourline5.exe` recognises which version the player has and
   starts the matching engine (section 5).
3. **DLC must work offline.** There are no PSN services, and no purchase or
   download inside the game. DLC the player owns and installed works once
   imported (section 4, "DLC offline").

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
| GT5 update PKGs | user-supplied, 2026-11-15 | **02.11 only**: `UP9000-BCUS98114_00-0000000000000000-A0211-V0100-PE.pkg`, 46,668,464 bytes. Extracted with the runtime's `tools/pkg_extract.py` into `input/v211/pkg/` (ignored): 152 files. `PARAM.SFO`: `APP_VER` 02.11, `TARGET_APP_VER` **02.10**, `PS3_SYSTEM_VER` 04.1000. It holds `EBOOT.BIN`, `TROPHY.TRP`, `PDIPFS/K/4D` (160 bytes, differs from 01.04's) and 145 files under `PDIPFS/9/`. It installs only on top of 02.10. |
| Master mod archive | user-supplied, 2026-11-16 | `GT5_2.11_Master_Mod_2.0.0_(US-BCUS98114).zip`, 408,640,237 bytes: data only (`PDIPFS`, an optional debug-camera add-on, the two original files it replaces or reads, readme, packing log). Measured in progress-gt5.md, 0.1.9 |
| 2.11 decrypted executable | `input/v211/EMAIN.ELF` (ignored) | 24,926,384 bytes, from `rpcs3.exe --decrypt` on a temp copy of the package's `EBOOT.BIN` (9,509,920 bytes, compressed SELF). Entry `0x171E4C8` (01.04: `0x1899940`). Last executable section ends at **`0xFA63A0`** (01.04: `0x11752A4`). 722 imports across 43 modules (01.04: 702 across 41). New modules: `cellSync` and `cellSysutilAvconfExt`. New NIDs: `cellGcmSys` +4, `cellSpurs` +2, `cellSysutil` +1, `sceNp` +5/-1, `sys_net` +5. |

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

**The update chain (Sony's public list, `BCUS98114-ver.xml`, read 2026-11-15).**
It lists 27 packages, 01.05 to 02.17, each with size and SHA-1. Every
package up to 02.11 is listed as a separate download: 01.05-01.13 (9),
02.00 (1,191 MiB) and 02.01-02.10 (10). Together, 01.05-02.10 is about
4.1 GiB. 02.11's `TARGET_APP_VER` of 02.10 means the chain is needed in
order. Whether 02.00 targets 01.13 or an earlier version is read from its
own `PARAM.SFO` when it is here.

**Installing without RPCS3.** A package install writes the package's files
into `dev_hdd0/game/<id>`; `pkg_extract.py` does the same. Extracting
01.05 to 02.11 in order into a separate 2.11 game folder, with each SHA-1
checked against Sony's list, gives the installed files without touching
the 01.04 RPCS3 install. RPCS3 is needed only to decrypt the executable,
which is already done. Any patching of installed data that the game does
itself on its first boot after an update still runs in the port, and has
yet to be measured.

**Installed, 2026-11-15.** The user supplied all 21 packages (01.05-02.11,
in `D:\Games\ROMS\PS3\GT5 Updates`). `scripts/gt5_updates.py check` passes
for all of them:
- sizes as in Sony's list;
- SHA-1 as in Sony's list, which covers each package without its last 32
  bytes (those hold the same digest and 12 zero bytes);
- every `TARGET_APP_VER` is the version before it, and 01.05 names none.

`scripts/gt5_updates.py install` then extracted them in order over the 01.04
disc package's 1,330 files, into `input/v211/root/gamedata/dev_hdd0/game/BCUS98114`
(ignored; the root also holds `input/EMAIN.ELF`, the 2.11 executable, so
`GT5P_ROOT` can name it):
- result: 21,664 files, 3.88 GiB;
- largest packages by file count: 02.00 5,325, 01.10 4,126, 01.05 2,018,
  02.02 2,007.

`volmap.py convert --dry-run` on that `PDIPFS` finds only 123 files
(0.03 GiB) that copy a `GT.VOL` range. The updates' data is new, so the
2.11 version costs about 4 GB that records cannot remove. Its own
first-boot install of `GT.VOL` copies has not run yet. Pins:
`config/gt5-updates.json`. Not yet measured: whether 2.11's first boot
patches installed files.

**Bring-up, 2026-11-15 (M2-M5 partly).** `BCUS98114-v211` in
`config/targets.json` holds only lift inputs measured on 2.11:
- `code_end` `0x00FA63A0`;
- `setjmp`/`longjmp` at `0x00BD6F94`/`0x00BD70B8` (from `find_setjmp.py`);
- the policy module, byte-identical, at `0x016A9000`;
- 38 job bodies (from `find_bodies.py`);
- 4,949 + 23 seeds.

01.04's hooks, screen files and addresses are not carried over.
`build/gt5_211.exe` boots the installed 2.11 game folder through the
warning, the logo, the first-boot install (accepted by `PAD_AUTO_INSTALL`,
stored as records), save creation and the opening movie. The logo-to-menu
stall is now fixed: MAIN-memory RSX reports were never published to guest
memory, leaving GPUSyncArray waiting on their completion words. The private
2.11 build reaches the animated main menu; the rollback reproduces the stall.
See [menu-stall investigation](gt5-v211-menu-stall.md) for evidence and limits.

Fixes needed on the way:
- **Sound jobs.** 2.11 changed `socisynth`, `socepdideq`, `socepdidist`
  and `socepdirta`. A missing capture leaves the sound chain's guard closed
  and the window white. `scripts/spu_job_images.py` lists the images from
  the executable and fingerprints them; `SPU_DUMP_MISS` captured the
  size-0 ones.
- **Movie.** The YUV-to-ARGB task ELF changed: `taskdump` gained
  `4C414F6C...`.
- **Seeds.** The block shared by `0x009D9660` and `0x009DA95C` needed its
  labels seeded.
- **argv block.** The 64 KB block at `0x00F00000` wiped 2.11's jump table
  at `0x00F03300`. It now moves per target (`argv_base`, 2.11
  `0x01F00000`).
- **SPURS workload flag** (runtime). `cellSpursGetWorkloadFlag` and
  `_cellSpursWorkloadFlagReceiver` are new in 2.11, and the render
  workload (wid 4) is the receiver. Added: the flag word at +0x6C, the
  receiver at +0x77, and a kernel kick that consumes the flag
  (`SPURS_WKL_FLAG_KICK=0` off).
- **Syscall 350** (runtime). `sys_memory_allocate_from_container` was a
  stub (`SYS_MEMORY_FROM_CONTAINER=0` old).
- **Audio.** The extended-zero fix and native images are mapped to 2.11's
  sound images.

Still unhandled imports (logged once each): `cellSysutilAvconfExt`
`0xFAA275A4`, four new `sceNp` and five `sys_net` NIDs.

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
4. **For DLC:** the player's own DLC, installed and licensed in their
   RPCS3. That means the DLC `.pkg` files installed, plus the player's
   `.rap` licence files under RPCS3's `dev_hdd0/home/<user>/exdata`. Section
   4, "DLC offline", covers how these are imported. Licences never go into
   this repository or the player ZIP.

### Identity to pin

`config/pt5-player.json` pins one executable (`elf_sha256`, `elf_bytes`) and
the disc package. 2.11 needs its own entry: the 02.11 ELF hash and size, the
expected `APP_VER` (hypothesis: `02.11`; any other value is refused), and the installed data's file count
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
| `ui_mask_patch.py`, `ui_mask_sites` (`0x008F53B8`, 01.04's `0x00A5D384`/`94`) | found by the 01.04 shape (W/2, H/2 from `[r30+8]`/`[+0xC]`, three `- fpr[30]` x edges); identical registers | low |
| `spu_native_patch.py`: `spu_native_hooks` (`b0196FF40` `0x4C14` `gt5p_spu_clear_words`), `spu_audio_round_zero_calls` (`j9804ABA8` `0x4E8`) | bodies are named by load address: if the body bytes are identical, rename to its 2.11 address and keep the offset; if not, re-find the loop and re-run `spu_clear.py` and `audio_coefficients.py` | low if identical, high if not (wrong hook = silent corruption) |
| `spu_regpromo.py`, `spu_regpromo` | off by default (measured slower); leave the key out | none |
| `spu_region.py`, `spu_regions` (two loop sets in `b01697100`) | same as native hooks; exactness re-checked with `spu_loop_bench.py` on new captures | medium |
| `spu_fma_images` (four bodies: fingerprint, image id, EA, span) | fingerprints are content; image ids are registry order and EAs move, so regenerate from the new registry; CMake refuses a fingerprint mismatch | low |
| `audio_native_images` (`jD58063CD`, `jEB79FDFC`, `jF523F26F`, `jFDFFAAC1`), `audio_extended_zero_images` | content-named job images; check they are still the audio DSP images (`audio_gt5.py`, `audio_extended.py`) | low if unchanged |
| `return_gap_seeds` (Phase 43, in the SPU lift) | automatic | low |

### Runtime addresses and measured game data

| Key (v104) | Feature | 2.11 method | Risk |
| --- | --- | --- | --- |
| `aspect_16_9_float` `0x019133B0`, `ui_aspect_16_9_float` `0x01914A48`, `scene_camera_aspect_16_9_float` `0x018EB70C` | 21:9 (Phase 41) | **done (0.1.9):** 2.11 `0x0148EF40`; `0x014941CC,0x014941D0`; `0x014196C8,0x014196F4` (2.11 reads constants by `lis`/`lfs`, not the TOC, and keeps two copies of the last two; see progress-gt5.md) | checked live |
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
service must answer "offline" cleanly rather than be implemented. The one
exception is the local licence check for DLC (section 4, "DLC offline").

**Measured for v104 (static, `analysis/gt5v104/hle_stubs_named.json`):**
01.04 already imports `sceNpDrmIsAvailable` and 30 `sceNpCommerce2`
functions, along with `cellGameContentPermit`/`cellGameDataCheck`. The
isolated runtime has no handler named `sceNpDrm*` (by name search). It
decrypts EDAT files on open (`libs/filesystem/edat.c`, `PS3_EDAT`). That
covers free-licence EDATs without extra input, and paid ones given a RIF key
(`PS3_RIF_KEY` or a `<file>.rifkey` sidecar). It deliberately cannot turn a
`.rap` into that key.

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

**Measured (2026-11-16), Master Mod 2.0.0a US:** data only, as hypothesised.
It replaces one install file (`PDIPFS/K/4D`, the volume header) and adds 696;
the new header points at a new table of contents (`9/57/QB`). The runtime
overlay `PS3_FS_OVERLAY=<dir>[;<dir>]` (item 2 below, now in `cellFs.c`)
loads it without touching the install: banner, debug menu (Select) and an
Arcade race work. Four screen markers change with it. The launcher installs
and removes mods and F1 picks one (setup-v211.md, "Mods"). See
progress-gt5.md, 0.1.9.

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

### DLC offline

**Goal.** Owned DLC works with no network. GT5's in-game shop and
re-download rely on PSN, which is gone. The game must find the installed
packs, accept their licences and read their content.

**What is not known yet (hypotheses to measure in M11):**

- **Where the DLC lives.** Hypothesis: each pack installs as `PDIPFS` files
  or EDATs under `dev_hdd0/game/BCUS98114/USRDIR`, the way RPCS3 places
  them. Part of some packs may already be in the 2.x update data, with
  the pack acting only as a licence.
- **How 2.11 detects DLC.** The candidates are a directory scan,
  `sceNpDrmIsAvailable` on each pack's EDAT, a licence file under
  `exdata`, or the game's own database.
- **Which licence type the packs use.** Type 3 ("free") EDATs decrypt
  already. Paid ones (type 1/2) need the player's own licence.
- **What the mod's "DLC unlock" does.** It may make DLC items visible
  without packs installed, which would then fail on content reads, or it may
  need the packs.

**How to measure.** First statically: imports, and strings such as
`addcont`, `.edat`, `exdata` and the DLC content ids in the 2.11 ELF. Then
a 2.11 RPCS3 install with the player's DLC: list the files each `.pkg`
added, read their NPD headers (licence type, content id), and log a first
boot in RPCS3 to see which files and which `sceNpDrm*` calls happen. In the
port, `PAD_FILE_LOG=2` and `PS3_HLE_TRACE` do the same.

**Design (proposal).**

1. **Import, not purchase.** Setup imports what the player's RPCS3
   already holds: the DLC files under the game folder, verified by count and
   size like the base data. There is no DLC `.pkg` install route at first;
   the update-PKG route (T17) can take DLC packages later.
2. **Licences without keys in this project.** The launcher must not carry
   PS3 keys (the existing rule), so it cannot turn a `.rap` into a key
   itself. In order of preference:
   - **a. Decrypt at setup with RPCS3.** RPCS3 decrypts each DLC EDAT with
     the player's `.rap`, and setup stores plain files in storage, as it
     does for the ELF. The runtime then reads plain files and its EDAT path
     is never used. Hypothesis: RPCS3's *Decrypt PS3 Binaries* accepts
     `.edat`/`.sdat`. Verify this before relying on it; if it does not,
     use b.
   - **b. A per-file RIF key the player supplies.** This is the runtime's
     existing `.rifkey` sidecar, made by a third-party tool the player runs
     themselves. It works, but it is a manual step, so it is a fallback only.
3. **An offline licence answer.** `sceNpDrmIsAvailable` (and any other
   `sceNpDrm*` 2.11 imports) answers OK only for content whose file is
   present and readable. With a missing file it answers with the real
   "no licence" error, so the game hides the pack as a console without it
   would. `sceNpCommerce2` answers as a console that cannot sign in: the
   shop says it is unavailable, and nothing hangs. Each behaviour gets an
   env switch (proposed `PS3_NPDRM_LOCAL=0` restores the generic stub).
4. **Storage.** DLC sits with the 2.11 data under `targets/BCUS98114-v211/`
   and is verified separately. The F1 page and launcher list the packs
   found. Removing a pack is a launcher action, not a manual file delete.
5. **With the mod.** The overlay (item 2 above) applies to DLC paths as
   well. If the mod's unlocks expose packs that are not installed, that is
   the mod's behaviour, recorded rather than fixed.

**Legal and distribution.** Unchanged rules: no game files, update
packages, mod files, DLC, keys or `.rap`/`.rifkey` files are tracked, packaged or
downloaded by this project. Decrypted DLC stays in the player's own storage,
like the decrypted ELF. The player supplies the mod archive and
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
| Storage | one storage root, `targets/<target>/` underneath for `input/`, `gamedata/` (saves, database, update and patched files), `shadercache/`; saves and progression are per target because a 2.11 save cannot go back to 01.04 (hypothesis: the game upgrades saves). The ~6.2 GiB of first-boot copies of `GT.VOL` are not stored per target at all ("Storage: the first-boot install is a copy of GT.VOL" below) |
| Switching | both versions set up; F1 GAME > "Game version" (01.04 / 2.11) restarts through the launcher (`src/pt5_restart.h`) into the other engine with that version's storage, saves and settings. Shown only when both are set up. The launcher remembers the last one |
| Shader caches | per target. Pipeline keys are content hashes, so sharing may work, but learned menu/race sets, the race pack and the fast-lookup table are per executable and data; measure overlap later before sharing |
| PGO | one profile per target, made from that target's lift (`pgo.py` takes `--target`) |
| Launcher | one `tourline5.exe` for both. Setup reads the selected install's `PARAM.SFO` and ELF hash: 01.04 selects `BCUS98114-v104`, 02.11 selects `BCUS98114-v211`, and anything else is refused by name, so 2.17 says "GT5 2.11 is required" and is never half-accepted. `setup.ini` `target=` (already present) records the result. Both versions may be set up side by side, each in its own storage subtree, with a version choice on the launcher page. `pt5-player.json` becomes a map of targets. F1 restarts and crash reports carry the target |
| Release | **one player ZIP** holding the launcher and both engines (`gt5.exe`, `gt5_211.exe`; each about 176 MB before compression), each PGO-built from its own profile. `clean_build.py` lifts and builds every target listed in the release config and `package_player.py` packages them all. Until M8 the 2.11 engine is labelled "experimental" in the launcher, and a release blocks if it fails M6 |
| Setup route for 2.11 | first: import an existing RPCS3 2.11 install plus `EBOOT.elf` (the existing import path, with `APP_VER` 02.11). Later: install a folder of the player's update PKGs in order with the launcher's RPCS3, pinned by hash, as the disc route does for `DATA000.PKG` |

### Storage: the first-boot install is a copy of GT.VOL

**Measured 2026-11-14 (read-only).** The method was GTToolsSharp 5.3.5
`listfiles` on the disc's `GT.VOL` and on RPCS3's 01.04 `PDIPFS`, followed by
byte comparison. RPCS3's 01.04 install has 16,450 files under `PDIPFS`, which
fall into two groups:

- **15,226 files (6.24 GiB) are raw byte copies of `GT.VOL` extents.**
  - Each is the volume entry with the same file index. Its size equals the
    entry's compressed size.
  - Each sits in `GT.VOL` at `649,216 + SegIndex * 0x800`.
  - The first and last 4 KiB of every one of the 15,226 match, and 40 random
    files match in full.
  - No decryption or decompression is involved: the game's install copies
    the stored bytes unchanged.
- **1,224 files (135.6 MiB) named outside the volume's table of contents**
  (`9/..`). These are the 01.04 package's own files.

So the 6.5 GB install is about 6.2 GiB of data already on the disc, plus about
140 MB of real content and the database. For 2.11, GTToolsSharp's
documentation says updates from 1.07 onwards carry binary patches that the
game applies to installed files. Some 2.11 files will therefore differ from
`GT.VOL`. Hypothesis: most will not.

**Proposal: a virtual install (runtime, switch-gated).** Copied files are
served from `GT.VOL` instead of being stored.

1. **Record during the game's own first-boot install.** When the guest
   writes a `PDIPFS` file whose bytes equal the `GT.VOL` range it just read,
   the runtime stores a small record (file name -> `GT.VOL` offset, length,
   hash) instead of the bytes. Any write that does not match is stored as a
   real file. This needs no keys and no parsing of the encrypted table of
   contents: the game's own reads and writes supply the mapping.
   (Hypothesis: the install reads and writes in matching chunks; check this
   with `PAD_FILE_LOG=2` on a fresh install.)
2. **Serve reads from the record.** Open, stat and read of such a file map
   to the `GT.VOL` range. The disc is already opened by absolute path
   (Phase 39). If the disc is moved or missing, setup says so; it does not
   fail at random.
3. **Copy on write.** When the game writes to or patches a recorded file
   (2.11's binary patches), the runtime first turns the record into a real
   file.
4. **Existing imports.** Setup can turn an imported RPCS3 install into
   records by the same byte comparison used above. It needs no table of
   contents: it hashes each imported file and finds the matching `GT.VOL`
   range from one read of the disc.

**What a second version then costs.** With records, 01.04 and 2.11 take no
space for the ~6.2 GiB of copies, and the records are shared. Each version
stores only its package or update data, its patched files, its database and
its saves. For 01.04 that is about 150 MB. For 2.11 it is the update data
(hypothesis: around 3 GB, from the EU 02.07 install's 2.9 GB) plus patched
files. Without this work, a second version costs about another 6.5 GB plus
the update data.

**Simpler interim option: NTFS hard links between the two versions'
identical files.** This is only safe with copy on write, because a write
through one link changes the file for both versions. It saves the duplicate
but still uses the 6.2 GiB once, so the virtual install is preferred.

Switches: `PS3_FS_VOLMAP=0` stores real files as today, and `=2` checks each
served read against a real copy when one exists.

**Status (2026-11-15, T23 in progress).** Both ways are built. The game's
own first-boot install stores records as it runs, and is accepted
automatically (item 1; see "The first-boot install records as it runs"
below). The launcher converts installs that already exist (item 4).

- **Runtime** (`libs/filesystem/volmap.c` and hooks in `cellFs.c`, in the
  compatibility patch).
  - A record is 256 bytes: magic, offset, length, CRC-32, the first and
    last 16 bytes, the volume's size, and the volume's guest path
    (`/dev_bdvd/PS3_GAME/USRDIR/GT.VOL`).
  - Read-only opens read a window of the volume. Open checks the volume's
    size and the head and tail bytes (`PS3_FS_VOLMAP=2` also checks the CRC).
  - stat and fstat report the range's size. Writes and `ftruncate` on such
    a descriptor return EBADF.
  - Opening for writing without `O_TRUNC`, or truncating by path, first
    turns the record back into the real file, atomically. `O_TRUNC`,
    `O_EXCL`, rename and unlink act on the record itself.
  - A changed or missing volume returns `CELL_EIO`.
  - Measured in two recent runs: normal play opens every `PDIPFS` file with
    flags `0x0` (about 490 opens a run), so play never turns a record back
    into a file.
- **Converter.**
  - `scripts/volmap.py` and `launcher/volmap.h` use the same method. They
    read each file of at least 4 KiB once to get its first 64 bytes, scan the
    volume once at 0x800 steps, and verify every match in full before
    replacing the file.
  - Dry run on RPCS3's 01.04 install, which changed nothing: 12,233 files
    (6.23 GiB) would become records, in 32 s. 905 files over 4 KiB matched
    nothing (the package's own files), and 3,312 smaller files are left as
    they are.
- **Launcher.**
  - Import writes records instead of copying matched files, so the import
    also stops needing that space.
  - Before play, `link_installed_data` converts in place whenever the
    ordinary files changed since the last pass (stored in `volmap.ini`).
    That covers the game's own first-boot install and installs set up
    before this change.
  - Only an engine that declares `volmap=1` in `pt5-release.ini` gets records.
- **Test.** `scripts/test/volmap_fs.py` builds a synthetic disc and install
  and runs the converter (convert, idempotence, restore, verify). It then
  runs the real `cellFs.c` and `volmap.c` in a harness covering reads,
  seeks, stat, copy on write, truncation, refused writes, a changed or
  missing volume, and modes 0 and 2. It passes, and fails as expected when
  records are off.
- **In game (2026-11-15, run `vm1`).** A private copy of the 01.04
  install (`scratch/vmroot`, 6.7 GB) was converted with `volmap.py`: 12,406
  files became records (6.26 GiB) in 98 s, leaving 435 MB. The test exe was
  linked from the main checkout's 0.1.5 objects, with `cellFs.c` and
  `volmap.c` recompiled from the private runtime (`scratch/vmbuild`). It
  was run with a cold shader cache, `GT5P_ROOT` set to the copy and
  `GT5P_GAME_DIR` set to the disc dump.
  - `gt5boot.ps1 -Route race` reached the menu (29.3 s), then Arcade, the
    track and car lists, race loading, the grid and the race start
    (+58.0 s). The race at High Speed Ring draws normally, with full frame
    rate once shaders are built.
  - Records were served from `GT.VOL` (the first 8 `[volmap] Open` lines
    are logged). No `CELL_EIO`, nothing turned back into a file, and no
    `.vmtmp` files were left.
  - One failed open, `PDIPFS/K/7M`, also appears in the 0.1.5 release
    smoke run without records.
  - Not measured: load time against full copies on a matched shader cache.
- **The first-boot install records as it runs (2026-11-15).** Item 1 above,
  now built. The game's own install stores records instead of copies, and
  every player gets the install without being asked.
  - **What the install does (`PAD_FILE_LOG=2`, run `fb2`).** For each
    file it creates `PDIPFS/<name>.psctmp99999`, sizes it with `Truncate`,
    opens `GT.VOL` and the temp file, reads the entry and writes the same
    bytes, closes both and renames the temp file. Afterwards it opens the
    last file once read-only.
  - **Runtime: install recording** (`cellFs.c`, `PS3_FS_VOLMAP_INSTALL=0`
    off; needs `PS3_FS_VOLMAP` on).
    - A write whose bytes equal what the same thread just read from a disc
      volume (`/dev_bdvd/...`), contiguous from position 0, is not stored.
      The descriptor keeps the range, CRC and head/tail bytes.
    - At close, a file exactly that long and at least 4 KiB becomes a record.
    - Anything else first writes the remembered bytes for real and carries
      on as an ordinary file:
      - a write of other bytes;
      - a read, `ftruncate` or short `fstat` through that descriptor;
      - another open, truncate or rename of the same path;
      - a file left a different length.
  - **Runtime: the prompt** (`cellPad.c`, `PAD_AUTO_INSTALL=<marker>`, set
    by `src/main.cpp` from `install_prompt_file` in `config/targets.json`;
    `GT5P_AUTO_INSTALL=0` off).
    - When the title's stat of `installed00.dat` fails, its "Data
      Installation Recommendation" dialog comes up with the cursor on Yes.
      Cross is pressed 3 s later, then every 2 s until the first
      `.psctmp` file is created (at most 15 s).
    - When the marker is created, "Installation complete" waits for OK. The
      title opens no files while it waits, so Cross is pressed 1.5 s later,
      then every 2 s until it opens any other file.
    - An installed boot finds the marker and nothing is pressed.
  - **Measured on private first-boot copies** (package files only, no save;
    test exes in `scratch/vmbuild` linked from the 0.1.5 objects):

    | Run | Install | Stored | Input |
    | --- | --- | --- | --- |
    | `fb2`: full copies (old runtime) | 427 s, 15,104 files | 6.5 GB | scripted Cross |
    | `fb3`: install recording | 342 s | 232 MB | scripted Cross |
    | `fb4`: recording + `PAD_AUTO_INSTALL` from `main.cpp` | 295 s | 240 MB | none |

    - `fb3` and `fb4` were compared file by file with `fb2` (records read
      from `GT.VOL`). All 31,540 files are identical, and 12,115 records
      stand for 6.20 GiB.
    - `fb4` went on unattended past "Installation complete" to the save
      creation and the GT5 logo.
    - The remaining time is the game's own per-file work and its 6.2 GB of
      reads from `GT.VOL`.
  - Test: `scripts/test/volmap_fs.py` now runs the install sequence through
    the real `cellFs.c`. A plain copy becomes a record. Changed bytes, a
    read through a second descriptor mid-copy and a file under 4 KiB are
    stored in full. With `PS3_FS_VOLMAP_INSTALL=0`, everything is stored in
    full.
  - The launcher's conversion remains for imported installs (RPCS3) and
    installs made before this change.
- **Still open.**
  - The launcher's own conversion (`link_installed_data`) has not been run;
    only `volmap.py` has, and the two share the format and method.
  - Not yet run: a race on data installed by recording (the `vm1` race used
    converted data, which has the same records).
  - A launcher restore path for `PT5_VOLMAP=0` after conversion. Until then,
    use `volmap.py restore`.
  - A player-setup test that drives the launcher's import with records.
  - Old setup backups under `<storage>\backups` are not converted.

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
| M9 | Multi-target product | one launcher and one ZIP carry both; setup detects 01.04/02.11 and refuses others; clean-machine install of each from the ZIP; F1 switches version with separate progression; first-boot copies of `GT.VOL` not stored (T23) |
| M10 | Master mod | overlay switch, mod's debug menu reachable, Arcade race with mod data |
| M11 | DLC offline | DLC layout and licence type measured; owned packs imported, listed and usable in a race with the network off; missing packs hidden cleanly; shop reports unavailable without hanging |

## Task status (2026-11-15)

Read from the commits, AGENTS.md and the live-check report:

- **done**: T1-T4, T6, T8, T9, T14, T14a, T17, T24.
- **partly done**:
  - T10: done 2026-11-16 (spin waits, aspect, listbox, top-menu word,
    Options marker, movie screen; glyphs needed nothing). See
    progress-gt5.md, 0.1.9.
  - T11: audio, native hook, regpromo, regions and FMA ported (see below);
    region speed at Eiger still to measure.
  - T13: 2.11 inputs live under `input/v211`.
  - T23: in game on 01.04; the launcher's own conversion has not been run.
- **not started**: T5 (a private masked matcher was used for the spin
  waits), T7, T12, T15, T16, T18-T22.

T9 details: the screen markers were measured from file-open times in three
runs and set in `config/targets.json`. They fire in route order: Arcade 34 s,
TrackList 44 s, CarList 49 s, Transmission 59 s, RaceLoad 64 s, RaceAssets
78 s, RaceGrid 79 s, RaceStart 85 s. `movie_screen` (skip movie) and the
Options marker are not measured yet. The race route is still a private
script (`scratch/release016-live/run-fix.ps1`), not `gt5boot.ps1`.

Milestones:

- **reached**: M0-M6.
- **open**:
  - M6: the race freeze (85-120 s) is fixed: created guest threads shared
    one TLS block (`PS3_TLS_FROM_CRT`, `PS3_TLS_PACK`; see
    gt5-v211-live-checks.md). Three 230 s races ran clean; a longer soak
    and full-race finish are still owed.
  - M7: race scope and race pack are in; F1 race pause and the rest are not.
  - M8-M11.

**Car list (2026-11-15).** 2.11 ran the car list at about 10 FPS, against
32-37 FPS on 01.04 (same machine, `gt5boot.ps1 -Route carlist`).

- **Cause found:** workload 4 (the car's render list) is woken by 2.11's
  workload flag between `ready=4` kicks. The runtime ran each flag kick on
  one lane and could not add lanes while that lane was busy, so 72% of
  dispatches were single-lane.
- **Fix:** `SPURS_WKL_FLAG_LANES` (runtime, default on; `=0` one lane) gives
  a flag kick the workload's full contention. Measured gain: about 15 FPS.
- **The earlier comparison used different cars.** 01.04's list opens on a
  Fiat 500, 2.11's on the Zonda R, the last car of 01.04's list. On the same
  car (Zonda R, 720p, 2x MSAA) 01.04 runs about 37 FPS and 2.11 ran 15-17.
  Other 2.11 cars ran 24-31 FPS.
- **Second cause (timeline, `PS3_TIMELINE`):** each frame's workload-4 work
  (about 96 ms of lane time) was shared by whichever lanes happened to be
  running. 2.11 kicks the workload before it appends the jobs, so most lanes
  found nothing and left within microseconds; one lane then ran the list
  alone (one lane ~96 ms, three ~32 ms). Later kicks only met a busy
  workload.
- **Fixes (runtime, `SPURS_LANE_JOIN`, default on, `=0` off):**
  - a kick, or the kernel's poll while the ready count stands, sends the
    pass's lanes that left back in, up to the ready count;
  - a workload-flag kick to the running workload is taken at once, with
    lanes joining, instead of starting an empty pass afterwards;
  - joins and a dispatch's setup share a lock: without it a kick could
    join a lane the kernel was also sending, and the pass waited for ever
    (seen once, at the track list);
  - `SPURS_LANE_RETRY=0` is now GT5's default (`src/main.cpp`): a lane that
    finds nothing leaves instead of sleeping 1 ms and putting back the
    ready unit the module took. Those retries held the pass open and the
    returned unit started an empty pass after it.
- **Result (Zonda R):** 15 -> 32-35 FPS. Per frame: pass 24.4 ms (01.04
  24.2), pass end to next kick 0.9 ms (01.04 2.1), kick to work start
  3.3 ms (01.04 0). The last is 2.11's own order: an SPU job of another
  workload raises workload 4's ready count about 3 ms after the PPU's kick.
  `SPURS_SPU_KICK_WAKE=1` gave about 1 FPS more, within noise; left off.
- **01.04 with the same runtime:** car list at the Zonda R unchanged (about
  37 FPS) and a High Speed Ring race at 60 FPS.
- New timeline kinds: `G` (lane woke), `j` (lane sent to join), `r` (SPU
  changed a ready count; needs `SPURS_SPU_KICK_WAKE=2` to watch the line).
- **Open, separate: white specks on car edges.** On 2.11 the Zonda R shows
  bright white pixels along its silhouette (windscreen frame, fender, sill,
  wheel arches). The first 01.04 shot at the same car did not show them,
  but the user sees them faintly on 01.04 too (2026-11-16): an older
  rendering fault that 2.11 makes more visible, not a 2.11 blocker. Not
  investigated yet.

**SPU tuning keys (T11, 2026-11-15).** 01.04's render bodies were matched to
2.11's by content:

| 01.04 body | 2.11 body | Match |
|---|---|---|
| `0x0196FF40` (car list) | `0x017BD780` | byte-identical; same in-memory fingerprint `0x76FF30204F3B9A9E` |
| `0x01697100` (Eiger) | `0x014FBC00` | 95.7% |
| `0x016B5B00` | `0x0151A400` | 87.4% |
| `0x016D2700` | `0x01538880` | 95.5% |

Ported to `BCUS98114-v211`:

- **`spu_native_hooks`**: the table clear in `b017BD780` at `0x4C14`.
  `scripts/test/spu_clear.py` now reads the selected target and passes 1,200
  parity cases on the 2.11 lift. Check mode in game reported no mismatch
  over about 300,000 clears.
- **`spu_regpromo`**: `b017BD780`, the same 14 functions. Still off by
  default (`GT5P_SPU_REGPROMO`). Check mode: 750,000 comparisons, 0 differ.
- **`spu_regions`**: `b014FBC00`. Every loop address is 0x50 lower than on
  01.04. The lifted loop functions match 01.04's apart from data addresses,
  which moved by 0x10-0x40. The merge finds the same shapes: 8 and 3
  functions.
  - An Eiger race with `GT5P_SPU_REGION=3` (merged and original alternating)
    ran until the known freeze with no visible fault.
  - Its speed is not measured: the freeze left too few race windows (now
    fixed; measure next).
- **`spu_fma_images`**: all four bodies, with fingerprints read from memory
  at boot. `[spu-fma]` now prints the value found on a mismatch. FMA stays
  opt-in, as on 01.04.

Car list after these, same build:

| Case | FPS | Frames / 5 s |
|---|---|---|
| defaults | ~15 | 80-86 |
| `GT5P_SPU_FMA=1` | 16-19 | 87-96 |

01.04 reaches 32-37 FPS on the same screen. The remaining gap is the extra
workload-4 passes per frame (above), not the body code.

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
| T14 | Launcher: version detection (01.04/02.11, others refused), per-target storage, 2.11 import route, version choice when both are set up | L | T2, T13 |
| T14a | Release: `clean_build.py`/`package_player.py` build and package both engines into one ZIP; per-target PGO; "experimental" label until M8 | M | T12, T13, T14 |
| T15 | Runtime `PS3_FS_OVERLAY` and launcher mod folder (done 2026-11-16: runtime, `launcher/mods.h`, F1 > Game > Mod/Mods; setup-v211.md "Mods") | M | T8 |
| T16 | Master mod test: inspect the archive first (data only?), then menu, debug menu, race (done 2026-11-16: data only; banner, debug menu, Arcade race) | M | T15, user's archive |
| T17 | Update-PKG install route in the launcher (pinned hashes, in order) | L | T14, T2 |
| T18 | Optional: LAN mode over `sysNet` | L | T16 |
| T19 | DLC triage: static strings/imports in the 2.11 ELF; per-pack file lists and NPD headers from the player's RPCS3 install; RPCS3 log of DLC detection | M | T1, T3, player's DLC |
| T20 | Check whether RPCS3 decrypts DLC EDATs from the command line or GUI with the player's `.rap` (setup route a); fall back to `.rifkey` (route b) | S | T19 |
| T21 | Runtime: offline `sceNpDrm*` answer from local files, offline `sceNpCommerce2` (switch-gated); unit tests with synthetic EDATs | M | T19 |
| T22 | Launcher: import, verify and list DLC; remove a pack; M11 race with the network off | M | T14, T20, T21 |
| T23 | Virtual install on v104 first: log the first-boot install's read/write pattern, then records for copies of `GT.VOL`, copy on write, `PS3_FS_VOLMAP` check mode; turn an existing import into records. Test: fresh install and an imported install both reach a race, with the ~6.2 GiB not stored | L | none (v104), T8 for 2.11 |
| T24 | F1 "Game version" switch and launcher restart into the other target; per-target saves and settings | M | T14, T23 |

## 7. Open questions for the user

Answered on 2026-11-14 (see the top): the target is 2.11 exactly; one
launcher and one ZIP; DLC works offline.

1. Which update packages do you have (01.05 to 02.11)?
2. May 2.11 be installed into a second RPCS3 folder, keeping the 01.04
   install as the reference for v104 work?
3. Both versions stay, switched in F1, and the launcher starts the last
   one used. Should 01.04 support ever be dropped?
4. Which master mod version, and has it been checked whether the archive
   holds anything outside `PDIPFS` (an EBOOT, patches)?
5. DLC: which packs do you own, and are they installed in an RPCS3 with
   their `.rap` files? Is a manual `.rifkey` step acceptable as a fallback
   if RPCS3 cannot decrypt DLC EDATs (section 4, "DLC offline")?
6. Is LAN mode wanted, or is offline play with the mod enough?
7. Other regions: the mod also supports EU/JP/Asia, and an EU 02.07 install
   is on this machine. Out of scope for now?

## Sources

- Polyphony Digital, Update Notice (2.11): https://gran-turismo.com/us/news/02_0006833.html
- GT5 2.17 coverage: https://onlineracedriver.com/2014/07/23/gran-turismo-5-v2-17-update-patch-released
- Nenkai, GT5 LAN Mode guide (master mod install, versions, regions): https://gist.github.com/Nenkai/f46bdce07c4ee019c57494319b292b30
- GTPlanet "GT5 Master Mod" thread (title seen in search, page not readable): https://www.gtplanet.net/forum/threads/1-9-2-gt5-master-mod.395844/latest
