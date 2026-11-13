<p align="center">
  <img src="assets/banner.png" width="1280"
       alt="Project Tourline 5. The PT5 logo centered over a mountain circuit at dusk. Unofficial PC port of Gran Turismo 5, built on your PC from your own game.">
</p>

# Project Tourline 5

**Gran Turismo 5: unofficial PC port (static recompilation)**

[![No game files](https://github.com/mathewdouglas/Project-Tourline-5/actions/workflows/no-game-files.yml/badge.svg)](https://github.com/mathewdouglas/Project-Tourline-5/actions/workflows/no-game-files.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue)](LICENSE)
![Status: early](https://img.shields.io/badge/status-early-orange)
![Windows x64](https://img.shields.io/badge/platform-Windows%20x64-0078D6)
![Direct3D 12](https://img.shields.io/badge/graphics-Direct3D%2012-555)

**[Getting started](#getting-started)** · **[Known problems](#known-problems)** · **[Progress](docs/progress-gt5.md)**

An unofficial port of **Gran Turismo 5** (PS3, US disc BCUS98114, update 01.04) to Windows x64. It is a static recompilation: the game's own code, translated ahead of time to native x64, then run with a Direct3D 12 renderer. No emulator runs while you play.

> **Status: early.** The game boots to the main menu with sound, and Arcade races run at around 55 FPS at High Speed Ring. Player builds are on the [Releases](https://github.com/mathewdouglas/Project-Tourline-5/releases) page; what changed in each is in [CHANGELOG.md](CHANGELOG.md). The investigation, with dates and measurements, is in [docs/progress-gt5.md](docs/progress-gt5.md).

This tree continues [Tourline Prologue](https://github.com/mathewdouglas/Tourline-Prologue). Both are built with [ps3recomp](https://github.com/sp00nznet/ps3recomp), from the [sp00nznet/GT5P](https://github.com/sp00nznet/GT5P) port of Gran Turismo 5 Prologue.

### What you need

- **Your own US disc copy** of Gran Turismo 5, **BCUS98114** (disc version 02.00), extracted to a folder: the one containing `PS3_GAME`, with `PS3_GAME\INSDIR` and the full `USRDIR\GT.VOL`. ISO files must be extracted first. The port runs the **01.04 update** executable from that disc's own `INSDIR/DATA000.PKG`, which is what a console and RPCS3 install before the first boot, so no update download is needed.
- **Windows 10 or 11, 64-bit**, a GPU with Direct3D 12, and an internet connection during setup (for RPCS3; see below).
- Nothing else. Setup fetches the official RPCS3 build on its own, uses it only to unpack the update on your disc, and deletes it. No game files, firmware or keys are included, and your disc folder is never modified.

## Getting started

### With the player download

1. Download the latest player ZIP from
   [Releases](https://github.com/mathewdouglas/Project-Tourline-5/releases)
   and extract it anywhere, keeping the folder together.
2. Run **tourline5.exe**. Keep the default storage folder
   (`%LOCALAPPDATA%\ProjectTourline5`) or choose an empty folder, including
   one on an external SSD. Saves, settings and shaders live there.
3. Select your extracted GT5 disc folder (the one containing `PS3_GAME`).
4. Choose **Prepare from my disc (recommended)**. That is all. Setup
   downloads RPCS3 (37 MB, the pinned official build), installs the 01.04
   update from your disc with it, decrypts the executable and copies the
   verified files into storage. An RPCS3 window opens and closes on its own:
   leave it alone. It takes under a minute, then RPCS3 is deleted again.
5. The game starts. Later launches go straight in; setup is remembered.

To use your own RPCS3 instead of the download, choose **Prepare from my disc
with my installed RPCS3**; setup presses its Install button and closes it for
you. Importing an existing 01.04 installation also works.

### With RecompLauncher

Import the player folder, open **Easy Setup**, drop your extracted disc
folder into the disc input, leave the other inputs empty, and press Play.
Preparation runs the same steps as above, unattended.
[RecompLauncher integration](docs/recomplauncher.md) has the details.

### Afterwards

**Change storage.cmd** moves storage to another folder, copying and verifying
it and keeping the original as a backup. **Change game files.cmd** selects the
game files again. [Setup, storage and game-file selection](docs/setup.md)
describes the full flow and its switches. Source builds put the same launcher
in `build\tourline5.exe`, also opened by **Setup and play.cmd**.

### Developer layout

The direct developer layout is also still supported:

1. Put the extracted disc where the port reads it: `input/pkg/PARAM.SFO` and `input/pkg/USRDIR/`, including `GT.VOL`. The port maps `/dev_bdvd/PS3_GAME/` to `input/pkg/`.
2. Decrypt the installed update executable, `dev_hdd0/game/BCUS98114/USRDIR/EBOOT.BIN`, to `input/EMAIN.ELF`. RPCS3's **Utilities → Decrypt PS3 Binaries** writes an ELF beside that file. Write `BCUS98114-v104` as the only line of `input/target`.
3. Copy a finished install to `gamedata/dev_hdd0/game/BCUS98114`. That folder is `dev_hdd0/game/BCUS98114` from RPCS3, after the game's own install has finished. The port maps `/dev_hdd0/` to `gamedata/dev_hdd0/`.
4. Run **build\\gt5.exe**. It finds this repository by looking upward from its own folder for `input\EMAIN.ELF`.

A first boot with only the update package's `PDIPFS` files, and with `GT.VOL` in place, runs the game's own install. That install copies the rest of the files out of `GT.VOL`. The screen order measured against RPCS3 is the warning, the install, the logo, adjust display, system files, the logo, the opening movie, then the menu.

The keyboard is used when no controller is connected: arrows, Z Cross, X Circle, A Square, S Triangle, Q/W L1/R1, 1/2 L2/R2, Enter Start, Tab Select. **F1** opens settings. They are saved in `gt5p_settings.toml` in the selected storage (the repository root for direct developer launches). **F3** opens the profiler.

F1 **Controls** has trigger dead-zone and response-curve options. **Feedback**
offers optional SDL vibration, Xbox trigger rumble and fixed DualSense trigger
resistance; these default off. The shared improvements adopted from Tourline
Prologue, their rollback switches and remaining GT5 prerequisites are recorded
in [the port review](docs/prologue-sync-review.md).

`scripts\get_sdl2.ps1` fetches SDL2 for more controllers. `scripts\get_ffmpeg.ps1` fetches FFmpeg for the opening movie. Without FFmpeg the movie stays black. The build copies both beside the executable. Keep `shadercache/`. It belongs to this machine.

**DXC** is the default shader compiler (player builds include it; source
builds run `scripts/get_dxc.py` and rebuild). FXC stays available under
F1 > Shaders. [Setup, cache separation and validation](docs/dxc.md).
F1 > Shaders > **Draw while compiling** (on by default) draws a stand-in
while a new shader compiles, instead of leaving the detail out.

## Known problems

- A locked 60 frames per second is not reached. Warm High Speed Ring races run at about 55 frames per second at 720p, Eiger Nordwand in rain at about 47 to 50, and runs vary. The first visit to a menu, track or effect still has some hitches while shaders compile; later visits use the shader cache.
- One-frame flashes (a purple road, grass on the road, noise in the mirror) were reported while thousands of shaders compiled. Copies that changed a texture the frame still being drawn had bound are now ordered with that frame. The flashes were not reproduced in the shorter local checks.
- A split-second blue flicker of the road, about 18 seconds into a race and during the rolling start, was reported. Burst captures after that report did not show it again.
- Online services are not part of this port.
- Only the US disc build BCUS98114, update 01.04, is the current target.

The measurement record is [docs/progress-gt5.md](docs/progress-gt5.md); release notes are in [CHANGELOG.md](CHANGELOG.md). [ROADMAP.md](ROADMAP.md) still describes the Prologue disc build.

## Building from source

The supported game is the 01.04 update executable above. A source build needs your own dump, Windows 10/11 x64, Visual Studio 2022 (C++ workload), LLVM 18+ (`clang-cl`), CMake 3.20+, Ninja and Python 3.10+.

Run `.venv\Scripts\python scripts/setup_runtime.py` first. It prepares the pinned runtime and this branch's GT5 compatibility patch in `../ps3recomp-tourline5`, without changing another project's runtime. `--check` verifies it. `scripts\build.cmd` uses that checkout unless `PS3RECOMP_DIR` is set. Per-build addresses live in [config/targets.json](config/targets.json). The 01.04 key is `BCUS98114-v104`. `code_end` is `0x011752A4`.

The HLE and SPU lift scripts use the same runtime, including `PS3RECOMP_DIR` overrides. Use that toolkit for all lifter commands and the build.

From your own disc and the decrypted 01.04 executable:

```bash
echo BCUS98114-v104 > input/target

python scripts/jumptables.py --write
python scripts/code_labels.py --write
T=${PS3RECOMP_DIR:-../ps3recomp-tourline5}/tools
python $T/find_functions.py input/EMAIN.ELF --json --seed-json config/extra_seeds_bcus98114_v104.json --output analysis/functions.json
python -c "import json; f=json.load(open('analysis/functions.json')); json.dump([x for x in f if int(x['start'],16) < 0x11752A4], open('analysis/functions_code.json','w'))"
python scripts/gen_hle_stubs.py input/EMAIN.ELF analysis/hle_stubs.json
python $T/ppu_lifter.py input/EMAIN.ELF --functions analysis/functions_code.json --output generated --code-end 0x11752A4 --header-name ppu_recomp.h --source-name ppu_recomp.c --hle-stubs analysis/hle_stubs.json
python scripts/setjmp_patch.py
python $T/gen_hle_nids.py --all --out src/gen/ppu_hle_nids.cpp
scripts/build.cmd -DGT5P_TARGET=BCUS98114-v104
```

`code_labels.py --write` is required. It seeds the computed-goto labels in the title's script VM. Without those labels the boot stops on a null object after the warning screen.

Generated sources stay in gitignored `generated/`. The committed SPU registry is `src/gen/spu_workloads_bcus98114_v104.c`. Captured SPU images are produced locally with `SPU_DUMP_MISS` and `python scripts/lift_spu_jobs.py`. After every SPU lift, run `python scripts/spu_native_patch.py`, then `python scripts/spu_regpromo.py`, then `python scripts/spu_region.py`. The job bodies come from the executable (`wws_job_bodies` in [config/targets.json](config/targets.json)). Re-run the lifts when ps3recomp's lifters change.

Then run `build\gt5.exe`.

| Target | What it is |
|--------|------------|
| BCUS98114-v104 | US disc, update 01.04. This is the executable the port runs. |
| BCUS98114 | US disc, version 01.00. A different executable. Kept, and not what a console runs after the update installs. |

## AI, licence, disclaimer

Project Tourline 5 is developed with AI coding agents. The maintainer reviews their work before merging it. Numbers in [docs/progress-gt5.md](docs/progress-gt5.md) are what that phase measured.

The port's own code, tools and documentation are [MIT](LICENSE). The licence covers nothing of the game itself. **Gran Turismo** is a trademark of Sony Interactive Entertainment. This is a fan project. It is not affiliated with, endorsed by, or connected to Sony Interactive Entertainment or Polyphony Digital.

**You need your own copy of the game.** This repository contains the port: source, tools and documentation. It contains no game code, data, assets, keys or firmware. `scripts/check_no_game_files.py` refuses those files on every push.

Gran Turismo 5 is © Sony Interactive Entertainment.
