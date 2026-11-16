# Optional GT5 2.11 setup

GT5 2.11 is an experimental second engine (first shipped in 0.1.6). It
reaches the menus and runs Arcade races, with the 01.04 PC features and
data mods such as GT5 Master Mod ("Mods" below). Setup accepts exactly
BCUS98114 01.04 and 02.11, and refuses 02.17 and other regions.

## Player flow

In RecompLauncher Easy Setup, select the supported extracted US disc as
usual. Leave the optional inputs empty for 01.04. To install 2.11, also
select **GT5 2.11 update packages**: one folder containing all 21 official
BCUS98114 packages, 01.05 through 02.11. Do not select Installed data at the
same time. Alternatively supply an existing complete 01.04 or 2.11
installation through Installed data, with its matching decrypted executable
or RPCS3. The installation's PARAM.SFO selects the engine.

Native **Change game files** offers a version choice. In a managed game,
**F1 > Game > Set up 2.11** closes the engine and opens the same native setup
flow under the launcher's storage lock. This button appears when the player
package includes a verified 2.11 engine and 2.11 is not already ready.
Cancel or failure returns to the previous engine. Direct developer engine
launches do not offer launcher-owned version controls.

Once installed, **F1 > Game > Game version** selects 01.04 or 2.11; choose
Restart to switch. An unavailable version cannot be selected. The launcher
validates the selected setup and engine again before switching, and remembers
the last successfully selected version.

**Carrying 01.04 progress over (0.1.9).** Saves stay separate per version.
Before 2.11 starts, if it has no `BCUS98114-GAME` save and 01.04 has one, the
launcher asks once whether to copy the 01.04 save (`BCUS98114-GAME` and
`BCUS98114-BKUP`, each user folder under `gamedata/dev_hdd0/home`) into 2.11's
storage. The copy is staged and verified file by file; anything already in
2.11's save folders moves to `backups/save-211-*` first; the 01.04 save is
only read. The answer is kept in `targets/BCUS98114-v211/save-copy.ini`.
Silent runs never ask (the question waits for the next interactive launch).
`PT5_SAVE_COPY=0` never offers; `=1` copies without asking. Later copies:
`tourline5.exe --copy-104-save` ("Copy 01.04 save to 2.11.cmd"), which asks
before replacing an existing 2.11 save. Checked live: 2.11 loads an 01.04 save,
writes its own backup save, and its menus and Arcade work. A 2.11 save is not
copied back to 01.04: once 2.11 writes it, 01.04 may not read it.

PC settings are shared between versions. Existing 01.04 data stays in its
original storage layout; no migration is required:

| Storage path | Purpose |
| --- | --- |
| `input/`, `gamedata/`, `shadercache/` | Existing 01.04 executable, data, saves and caches |
| `targets/BCUS98114-v211/input/`, `gamedata/`, `shadercache/` | 2.11 executable, data, saves and caches |
| `gt5p_settings.toml`, `logs/`, `selection.ini` | Shared PC preferences, launcher logs and active-version selection |
| `tools-v211/` | Isolated package-installation tool and resumable staging; removed after success |

Both engines read the same selected disc in place. The existing virtual
install path handles verified copies of disc ranges. The updates themselves
contain about 3.88 GiB of additional files that cannot generally be replaced
by disc-range records. Package setup needs about 8.3 GiB free temporarily for
the isolated installation plus verified import.

## Installation and recovery

All package names, sizes and SHA-1 values are pinned in
`config/gt5-updates.json`; the native metadata is in
`launcher/update_packages.h`. SHA-1 covers each package except its final
32-byte footer, matching the measured Sony package convention. Tests check
the metadata copies agree. No game packages, firmware or keys are downloaded
or included.

Package setup uses its own pinned RPCS3 under `tools-v211`, installs the
disc's base package, then applies all 21 packages in order through RPCS3's
headless installer. One Tourline 5 progress window covers the chain; RPCS3
opens no installer windows. Cancellation stops only setup's owned process.
Completion requires the measured cumulative file count and total bytes plus
the expected installed APP_VER. No game is booted in RPCS3.

The pinned RPCS3 build aborts in its headless teardown after extracting files.
Setup accepts only exit 0xC0000409 with the specific `fixed_typemap.hpp:220`
and `Emulation object is unavailable (process teardown)` diagnostics, and
only after validating the complete expected output. Other failures stop.
Re-check this exception when updating the pinned tool.

The final executable is decrypted from a temporary copy and checked against
the pinned 2.11 size and SHA-256 before activation.

A retry verifies all packages again, then resumes a completed, matching
intermediate version. A partial or unrecognised staging tree is renamed and
preserved before starting a fresh staging install. No player RPCS3 installation
is upgraded. Failed imports retain the previous playable setup; replacement
game data/executables use the existing verified staging and backup mechanism.
The game's own first-boot install runs in the port after preparation.

`PT5_RPCS3_ARCHIVE` supports the pinned tool archive locally. With
`PT5_RPCS3_DOWNLOAD=0`, import an existing 2.11 installation instead of using
package setup. `PT5_KEEP_RPCS3=1` retains staging for diagnosis.

## Mods

Data mods (GT5 Master Mod and similar) are installed per game version by the
launcher (`launcher/mods.h`) and read by the runtime over the installed data
(`PS3_FS_OVERLAY`, `cellFs.c`: read-only opens and stats of
`/dev_hdd0/game/<id>/USRDIR/...` use the overlay's file when it exists; writes
go to the install). The installed data is never changed.

- **Player flow.** F1 > Game > Mods closes the game (exit code
  `pt5_restart::mods_manage`) and opens the launcher's mod manager: install a
  mod from a ZIP or remove one, then the game starts again. F1 > Game > Mod
  lists Off, each mod and each mod with one of its add-ons
  (`GT5P_MOD_CHOICES`, `GT5P_MOD_ACTIVE`); Restart hands the choice back
  (`pt5_restart::mod_select | index`). Both appear only when the launcher
  started the game.
- **Install.** Windows' `tar.exe` unpacks the ZIP into a staging folder. Only
  `PDIPFS` trees are kept: the top-level one (or one inside a single wrapping
  folder) is the mod, any other `<folder>/PDIPFS` is an add-on.
  `PDIPFS_ORIGINAL`, when present, must match the installation byte for byte
  (volume-range records are skipped): a mod for another version or region is
  refused. ZIPs with links or paths outside the folder are refused (tar
  refuses `..`), as is a second install of the same ZIP (SHA-256).
- **Layout.** `<version storage>/mods/<id>/PDIPFS`,
  `mods/<id>/addons/<folder>/PDIPFS`, `mods/<id>/mod.ini` (name, source,
  sha256, addons) and `mods/mods.ini` (`active`, `addon`). The version
  storage is the storage root for 01.04 and `targets/BCUS98114-v211` for 2.11.
- **Saves.** Turning a mod on from Off first copies the version's
  `BCUS98114-GAME`/`-BKUP` to `backups/save-before-mod-<time>`.
- **Command line.** `tourline5.exe --mods` (the manager),
  `--install-mod <zip>` (installs and selects it), `--remove-mod <id>`; each
  takes `--target=`.
- **Switches.** `PT5_MODS=0` turns mod support off (nothing loaded, no F1
  row). A `PS3_FS_OVERLAY` set before the launcher starts wins over the
  selection.
- **Known limit.** Master Mod replaces the files four of 2.11's screen markers
  name (Arcade, car list, race load, grid); in one Arcade run with it those
  never fired, while track list, transmission, race assets and race start
  did. So a race load is recognised later (at race assets), and leaving a
  race for the grid is not seen. Not measured further.

## Developer and release interfaces

Native setup accepts:

```text
tourline5.exe --recomplauncher-prepare --game DISC --updates=PKG_FOLDER
tourline5.exe --configure --target=02.11 --game DISC --game-data INSTALL --elf ELF
tourline5.exe --target=02.11 --check
tourline5.exe --target=01.04
```

RecompLauncher passes the optional package folder as one argument:
`--updates={input:updates211}`. It omits that argument when unselected.
Its preparation timeout is four hours. Native setup owns all content
validation and refuses conflicting package/import inputs.

Both CMake targets build the launcher. Source staging records each engine's
hash in a target-named section of `pt5-release.ini`. Player packaging accepts
`--build-211 BUILD --elf-211 ELF` alongside the ordinary 01.04 inputs to
produce one ZIP containing both engines; both builds must be Release and
their shared runtime DLLs must match. No ELF enters the package.

For a reproducible release, first run `clean_build.py --target
BCUS98114-v211 --engine-only` with its own private inputs and optional
profile, then pass that build and ELF through `--build-211` and `--elf-211`
to the normal 01.04 clean build. `--profile-211` records the secondary
profile's provenance. Profiles must come from each target's own sources and
lift. A normal release still requires the outstanding race/parity acceptance;
these options do not make the experimental engine release-ready.

## Checks

`scripts/test/player_setup.py` covers both identities, separate roots,
managed switching, 2.17 refusal, all 21 package stages using a fixture RPCS3,
failure/resume, malformed input and preservation of 01.04. It also retains
the existing storage, decryption, locking, reporting and rollback checks.
`recomplauncher_manifest.py` checks the optional input and metadata pins;
`player_package.py` checks the unified ZIP and both engine declarations.

Local validation on 2026-11-15 installed the real disc base and all 21 updates
in isolated temporary tool folders. Every headless stage matched its measured
file count and bytes. The production installer accepted the real final package
with the pinned teardown signature. The final imported ELF matched the pinned
2.11 identity. The F1 version choices were rendered and inspected in game.
Subsequent [live checks](gt5-v211-live-checks.md) validate F1 and managed
restart, but find nonfinite race audio and a race stall. No clean dual-engine
release or full race/parity acceptance has passed.
