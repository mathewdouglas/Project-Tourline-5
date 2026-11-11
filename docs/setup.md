# Project Tourline 5 setup

Run **tourline5.exe**. A source build puts it in `build/`; **Setup and play.cmd**
opens it too. In a player package, run the launcher at the top of the extracted
folder. Setup follows Tourline Prologue's storage and file-selection flow:

1. Keep the default storage, or choose an empty folder on another drive,
   including an external SSD. Default: `%LOCALAPPDATA%\ProjectTourline5`.
2. Select the extracted **GT5 US disc, BCUS98114, disc version 02.00**, or its
   `PS3_GAME` folder. The disc stays in place; `GT.VOL` is not copied.
3. Select **01.04 installed game data**: your RPCS3 folder or
   `dev_hdd0\game\BCUS98114`. This installation is copied and verified into
   storage. Close RPCS3 before importing. Source files are left unchanged.
4. Select the decrypted update's `EBOOT.elf`, or let your installed RPCS3
   decrypt a temporary copy of `EBOOT.BIN`. A matching ELF already beside the
   installed update is detected automatically.

GT5 needs the installed update as well as the disc. In RPCS3, first boot
installs the disc's `INSDIR\DATA000.PKG` (01.04). Select that installation;
the disc executable and newer updates are different builds. Setup checks
title/version metadata when available and the executable's exact SHA-256.
No game files, keys, firmware or RPCS3 are downloaded or included. An incomplete
GT5 install finishes in the game on first boot; a completed install can also
be imported.

Later launches use the remembered setup. **F1** opens settings; Restart goes
through the launcher while keeping storage locked. New profiles use 720p/2x
MSAA and the current DXC default. Existing settings in storage are preserved.

## Storage and later changes

**Open storage.cmd** opens your files in Explorer. **Change storage.cmd** has
Prologue's current-folder/custom-folder choice. It copies and verifies storage
before remembering the new location, keeping the old folder as a backup.
Nonempty destinations and overlapping folders are refused. A disconnected
drive leaves the saved selection intact; reconnect it before launching.

**Change game files.cmd** selects source files again. Imports are staged;
cancellation or failed copying leaves the previous setup selected. Replaced
installed data/executables are retained under `backups/`. Saves are separate
and are not replaced. Copying shows progress and offers cancellation. Linked
source files/folders are refused rather than traversed.

| Storage location | Contents |
| --- | --- |
| `setup.ini` | Remembered disc location and target |
| `input/EMAIN.ELF` | Verified imported update executable |
| `gamedata/` | Installed data, saves and guest writable files |
| `gt5p_settings.toml` | F1 settings |
| `shadercache/` | Locally generated shaders and pipelines |
| `logs/` | Play, setup and decryption diagnostics |

The locator is `%LOCALAPPDATA%\ProjectTourline5\storage.ini`. No administrator
rights are needed. As in Prologue, the current runtime requires reasonably
short paths representable in the Windows system language. Unsupported paths
are rejected before launch.

Developer files in the checkout remain in place; direct `build/gt5.exe`
launches still support that layout. To transfer a developer profile, close
the game and copy its settings, shadercache and `gamedata/dev_hdd0/home/` to
the corresponding storage locations before playing. Keep the source backup.

## Automation and packages

`--check` validates setup and engine without dialogs or guest execution.
`--setup`, `--choose-storage` and `--open-data` match the shortcuts.
`--set-storage "EMPTY FOLDER"` performs a noninteractive relocation. Scripted
setup, with no game launch:

```powershell
build\tourline5.exe --configure --game "D:\GT5\PS3_GAME" --game-data "D:\RPCS3" --elf "D:\GT5-update\EBOOT.elf"
```

`PT5_LAUNCHER_HOME` overrides the locator home. `PT5_USER_DIR` selects a profile
without changing the locator; remembered storage changes are then refused.
`PT5_STORAGE_PICKER=0` suppresses the first storage dialog. `PT5_SETUP_UI=0`
disables interactive setup for automation. The launcher owns storage locks
through play and F1 restarts. Direct engine launches retain their original
restart behavior unless `GT5P_LAUNCHER_RESTART=1` is explicitly set.

The build produces the launcher, engine, assets, shortcuts and matching
`pt5-release.ini`. Players need no Python/compiler. The existing release
packager accepts an unpacked destination as well as `--output FILE.zip`:

```powershell
.venv\Scripts\python scripts/release/package_player.py --build-dir build --elf input/EMAIN.ELF --runtime ..\ps3recomp-tourline5 --vc-runtime-dir "PATH\TO\Microsoft.VC143.CRT" --stage-dir build/player
```

Destinations must be new/empty. The strict allowlist includes the launcher,
engine, runtime dependencies/notices, glyph art and shortcuts; it excludes
game files, settings, caches, saves and logs.

The player folder also includes `recomplauncher.json`, which starts the native
launcher and exposes GT5 settings and storage/log folders in RecompLauncher.
See [RecompLauncher integration](recomplauncher.md), including the one-time
launch needed to establish the link for an existing relocated profile.

`scripts/test/player_setup.py` exercises the production launcher with synthetic
file identities and a fixture engine: import, metadata checks, source retention,
copy/commit failures, migration, settings, restart/locks, disconnected storage,
junction refusal and tampering. Production launchers were also checked against
real GT5 files read-only. No game instance was started for this validation.
