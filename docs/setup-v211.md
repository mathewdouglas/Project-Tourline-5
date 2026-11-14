# Optional GT5 2.11 setup

GT5 2.11 is an experimental second engine. It reaches the menu; race and
full feature parity are separate bring-up milestones. Setup accepts exactly
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
the last successfully selected version. Existing saves are not migrated.

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
