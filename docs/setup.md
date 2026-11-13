# Project Tourline 5 setup

Run **tourline5.exe**. A source build puts it in `build/`; **Setup and play.cmd**
opens it too. In a player package, run the launcher at the top of the extracted
folder. Setup follows Tourline Prologue's storage and file-selection flow:

1. Keep the default storage, or choose an empty folder on another drive,
   including an external SSD. Default: `%LOCALAPPDATA%\ProjectTourline5`.
2. Select the extracted **GT5 US disc, BCUS98114, disc version 02.00**, or its
   `PS3_GAME` folder. The disc stays in place; `GT.VOL` is not copied.
3. Choose **Prepare from my disc**. Nothing else is selected or clicked:
   setup downloads one pinned official RPCS3 build (37 MB, SHA-256 checked)
   into `<storage>\tools\rpcs3`, opens the disc's bundled
   `INSDIR\DATA000.PKG` in it, presses **Install** with "Precompile caches"
   unticked (Windows UI Automation), and closes RPCS3 once the package's
   1,335 files are all present. No update download, firmware or GT5 boot in
   RPCS3 is required.
4. Setup decrypts a temporary copy of `EBOOT.BIN` with that RPCS3, copies and
   verifies the required data into your chosen storage, and deletes the
   tools folder. A failed setup keeps it, so a retry does not download again.

**Prepare from my disc with my installed RPCS3** does the same with your own
`rpcs3.exe` (no download). Close other RPCS3 windows first; setup presses
Install and closes it for you. An existing standard-layout 01.04 install in
that RPCS3 is reused, and a different GT5 version or custom VFS there is left
unchanged.

Alternatively choose **Import an existing 01.04 installation** and select
your RPCS3 folder or `dev_hdd0\game\BCUS98114`, then the decrypted update's
`EBOOT.elf` or your installed RPCS3. This also supports completed game installs
and custom RPCS3 game-data locations. Existing source files are retained.

In **RecompLauncher**, drop only the extracted disc folder into Easy Setup
and leave the other inputs empty: preparation runs the route above unattended.
You can instead supply your own `rpcs3.exe`, or an existing 01.04 installation
and optional `EBOOT.elf`.
Play uses the same native launcher. Direct launches keep the built-in prompts.
See [RecompLauncher integration](recomplauncher.md).

The supported disc already contains the required 01.04 package. Its INSDIR
folder must be present; a dump containing only USRDIR is incomplete for this
setup route. The disc executable and newer updates are different builds. Setup checks
title/version metadata when available and the executable's exact SHA-256.
No game files, keys or firmware are downloaded or included. RPCS3 is not
included either: the only download is the pinned official build above
(`rpcs3_url`/`rpcs3_sha256` in `config/pt5-player.json`), and on Windows
without a 7z-capable `tar` also 7-Zip's pinned `7zr.exe`. An incomplete
GT5 install finishes in the game on first boot; a completed install can also
be imported.

If RPCS3 already contains another GT5 version, disc preparation stops before
installing over it. Use a separate RPCS3 folder or import matching 01.04 data.
With a custom RPCS3 `dev_hdd0` location, import that installation explicitly.
Package-installation logs are saved as `logs/install-package.out` and `.err`.

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
build\tourline5.exe --configure --game "D:\GT5"
build\tourline5.exe --configure --game "D:\GT5\PS3_GAME" --game-data "D:\RPCS3" --elf "D:\GT5-update\EBOOT.elf"
```

The first form uses setup's own RPCS3. Switches for it:
`PT5_RPCS3_DOWNLOAD=0` (no download: an rpcs3.exe or existing installation
is required, as before), `PT5_RPCS3_ARCHIVE=<file>` (a local copy of the
pinned archive, for offline setup; still hash-checked), `PT5_KEEP_RPCS3=1`
(keep `<storage>\tools` after setup), `PT5_RPCS3_AUTOCLICK=0` (the player
clicks Install), `PT5_RPCS3_NO_TAR=1` (always use `7zr.exe`). To move to a
newer RPCS3, update the pinned URL, SHA-256 and size and rebuild.

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
