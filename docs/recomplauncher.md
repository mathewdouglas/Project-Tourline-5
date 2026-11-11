# RecompLauncher integration

`launcher/recomplauncher.json` follows Tourline Prologue's integration at
`7d0ab12`, adapted to Project Tourline 5. Both source-build staging and the
release packager put it beside `tourline5.exe` at the player folder's root.
See RecompLauncher's [developer guide](https://github.com/elripalda/RecompLauncher/blob/main/docs/DEVELOPERS.md).

Play starts **tourline5.exe**, preserving GT5's disc/update selection, identity
checks, storage locks and managed F1 restarts. There is no `game` or `prepare`
section: the native launcher already imports and verifies BCUS98114's 01.04
installed data and executable. First-time setup still uses its dialogs.

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
It never starts a real game or changes a real player profile.

`scripts/test/storage_link_probe.cpp` and `storage_link.py --probe PATH` also
exercise the junction helper directly, including retargeting a disconnected
target, drive roots, preserving real folders and refusing network targets.

An actual RecompLauncher Developer tools import and player-page check remain
manual acceptance work. No catalogue registration or public release is made
by this change. Keyboard bindings, guided setup, saves configuration and
Big Picture-specific behavior remain as in Prologue's initial integration.
