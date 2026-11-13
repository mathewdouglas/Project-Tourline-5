# RecompLauncher integration

`launcher/recomplauncher.json` follows Tourline Prologue's integration at
`7d0ab12`, adapted to Project Tourline 5. Both source-build staging and the
release packager put it beside `tourline5.exe` at the player folder's root.
See RecompLauncher's [developer guide](https://github.com/elripalda/RecompLauncher/blob/main/docs/DEVELOPERS.md).

Play starts **tourline5.exe**, preserving GT5's disc/update selection, identity
checks, storage locks and managed F1 restarts. Easy Setup uses named `inputs`
and a `prepare` hook. Direct launches without RecompLauncher
still open the built-in storage/game-file dialogs when setup is needed.

## Easy Setup

Drop the following into RecompLauncher's Easy Setup inputs:

1. The extracted **BCUS98114 v02.00 disc folder**, or its `PS3_GAME` folder.
2. Your installed **rpcs3.exe** in **RPCS3 / Executable**.
3. Leave **Installed data** empty to prepare from the disc. Close other RPCS3
   windows first. Setup opens the bundled package in RPCS3: click **Install**,
   wait for completion, then **close RPCS3**. Setup then finds the files,
   decrypts a temporary copy and imports verified data. No GT5 boot is needed.

Alternatively provide existing **01.04 installed data** (or its RPCS3 folder)
in the optional input, plus a decrypted **01.04 EBOOT.elf** or `rpcs3.exe` if
not found there. This route supports custom RPCS3 game-data locations.

The supported disc includes the required 01.04 package at
`PS3_GAME/INSDIR/DATA000.PKG`; no separate update download is required.
ISO files must be extracted with INSDIR included. Newer updates, other
regions and the original disc executable are refused by native setup.
Disc preparation installs that package into RPCS3 before importing its data
and verified ELF. An existing different GT5 version or VFS configuration is
left unchanged; use a separate default-layout RPCS3 folder or explicit import.
Keep the disc folder available for play.

RecompLauncher runs `tourline5.exe --recomplauncher-prepare --game DISC
[--game-data=UPDATE] [--executable=ELF_OR_RPCS3]` before Play. This uses the
same staged copy, executable hash and commit/rollback checks as built-in
setup. It uses default or remembered storage without a picker, emits
`progress` lines while importing/verifying and exits without starting the
game. Missing or invalid inputs fail with stderr and `logs/setup.err`;
they never open another launcher setup dialog. RPCS3's own package installer
requires the Install click and closing RPCS3 afterward. The preparation timeout is two hours
to allow large imports on slower drives. No shared key or file destinations
are declared in the manifest: RecompLauncher passes selected paths directly.

After preparation, Play starts the native launcher normally. Existing setup
is remembered by both launch paths. Use Change storage.cmd to relocate it.
RecompLauncher reruns preparation when setup inputs or the installed version
change. Older RecompLauncher versions that do not support named inputs can
still use the built-in setup on Play.

The settings page edits `gt5p_settings.toml`: resolution, aspect, window mode,
MSAA, V-Sync, filtering, smooth colour, shader preload/compiler, skip movie,
button prompts, trigger response/dead zone and the three optional feedback
settings. Changes apply on the next launch; F1 edits remain in the same file.
`config.new` matches the native launcher's first-run preset: 720p, 2x MSAA,
DXC and preload enabled. Existing explicit preload-off settings are preserved.
GT5 skips the movie with a timed button press; Prologue's native opening-option
hook and its defaults-version migration are not used here.

## Selected storage

RecompLauncher cannot read the selected path from `storage.ini`. As in Prologue,
the native launcher maintains a directory junction at
`%LOCALAPPDATA%\ProjectTourline5\selected-storage`. The manifest selects settings,
logs and storage through it when present, otherwise the default folder.

Normal launches and setup/storage changes repair the junction while holding
the storage locks. A successful relocation retargets it; returning to default
storage removes it. Only the link is removed, never its target. Real folders
and other reparse-point types are preserved. A disconnected target can be
retargeted after reconnection. Network targets are unsupported by junctions.
Link failures are logged to the selected storage's `logs/storage-link.err` and
do not stop play. `PT5_STORAGE_LINK=0` leaves the link alone. `PT5_USER_DIR`
runs never change it; `--check` remains read-only.

For an existing relocated profile, run the updated native launcher once before
editing settings in RecompLauncher, so it can establish the junction. While an
external drive is disconnected, RecompLauncher falls back to the default
folder; the native launcher still refuses play until the selected drive returns.
The Open Storage Folder and Open Logs buttons follow the same path selection.

## Validation

```powershell
.venv\Scripts\python scripts/test/recomplauncher_manifest.py
.venv\Scripts\python scripts/test/player_package.py
.venv\Scripts\python scripts/test/player_setup.py
```

The manifest test exercises every choice with a TOML line editor, checking
accepted keys/values, preservation of other lines and first-run preset parity.
Package tests check ZIP and CMake staging and refuse an engine-only start.
The setup test compiles the launcher and fixture engine into private scratch
folders, checks junction repair/removal and rollback/override behavior, and
retains the existing setup, migration, restart, locking and rejection checks.
It also runs the actual manifest's prepare arguments with optional inputs
present/absent, automatic ELF/RPCS3 discovery, progress output, failed
decryption, wrong executables and subsequent native Play. GUI setup is enabled
in those fixtures to catch accidental dialogs, and preparation must not run
the game engine. Failed imports preserve the previous setup and settings.
It never starts a real game or changes a real player profile.

`scripts/test/storage_link_probe.cpp` and `storage_link.py --probe PATH` also
exercise the junction helper directly, including retargeting a disconnected
target, drive roots, preserving real folders and refusing network targets.

An actual RecompLauncher Developer tools import and player-page check remain
manual acceptance work. No catalogue registration or public release is made
by this change. Keyboard bindings, saves configuration and Big Picture-specific
behavior remain as in Prologue's initial integration.
