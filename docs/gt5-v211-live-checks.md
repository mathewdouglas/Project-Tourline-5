# GT5 2.11 live checks (2026-11-15)

The 0.1.6 candidate is not ready for release. The normal 2.11 race route
reaches playable High Speed Ring, but produces invalid audio and subsequently
stops presenting. These are bring-up failures, not a completed race pass.

## Build and method

Tested commit `d48e59c` with the existing non-PGO Release 2.11 lift and runtime
library, relinked with the current F1 implementation. Engine SHA-256:
`745fac9179a8d65911c5dd90e2fbe2e289b7644d27c7b3da26023b17f2cbcf0b`.
The executable input matched the pinned 2.11 identity. The committed runtime
patch reverse-checks against the private runtime used by the build.

The separate, uncommitted renderer-error changes were not compiled into this
engine. These checks do not validate their fatal-error/report/restart path.

Game data and saves were copied into an isolated scratch root. The original
installation and saves were preserved. Runs were sequential, at 720p, 2x MSAA,
DXC, with finite/nonfinite audio diagnostics and `AUDIO_NONFINITE_SCRUB=0`.
The race script used runtime milestones and timed button presses; unmeasured
01.04 screen-file markers were not reused for 2.11.

`PrintWindow` captures retained an old logo even while the visible game advanced.
Direct visible-window captures established the grid, race and HUD states below.
Window titles and draw counts alone were not accepted as proof of progress.

## Results

- The opening movie renders and produces finite, nonzero audio.
- Two later scripted boots reach `MusicDecode` at about 25.4 seconds, using
  the normal synthetic report mask (`GCM_REPORT_FULL=2`). Earlier scripted
  runs stayed on the logo; the input/timing difference is unresolved. These
  checks do not establish reliable cold-boot coverage.
- The normal race route passes the menu, car selection and starting grid,
  then draws High Speed Ring with cars, road, scenery, HUD and rear view.
  Visible captures show lap 1/2 advancing from 1.316 to 17.866 seconds and
  the car moving from 107 to 38 mph as the script intermittently accelerates.
  This was not a completed lap or a controlled performance measurement.
- Race audio fails. The normal run logs `input_nan=768`, `mixed_nan=256`
  and `mixed_inf=0`; repeated peak records also contain NaNs. The menu's
  healthy audio does not establish race-audio correctness.
- The normal run subsequently logs no present for 3 seconds, then 11 seconds,
  after 4,759 presentations. The later stall snapshot has a drained FIFO; the earlier snapshot is parked
  in a mapped command buffer.
  The test ends its own process at 145 seconds. No cause is established here.
- A separate diagnostic run with `GCM_REPORT_FULL=0` also reaches the grid
  and race introduction, then stalls and produces NaNs. It does not justify
  changing the normal report policy; no runtime default was changed.
- The F1 smoke test passes its stored-setting assertions for video, shader,
  input and feedback choices. Game version displays 2.11, lists both versions
  and rejects unavailable 01.04 without changing the active selection.
  The unavailable-version check uses explicit launcher-state environment
  values; it is not a live switch into a second real engine.

The real launcher-managed F1 restart also passes. Changing 720p to 1080p,
then choosing Restart, replaces the engine process while retaining the same
launcher process and remembered 2.11 target. The new engine displays 1920x1080
and reads the saved setting. This uses a real pinned executable, prepared
private game data and the production launcher; it does not validate switching
between two real engines or restarting after a fatal renderer error.

## Evidence and next work

Private evidence is under `scratch/release016-live/`: `provenance.json`,
`report-zero.*`, `report-two.*`, `race-default-repeat.*`, their runner logs,
`default-race-90.png`, `default-race-125.png`, and `settings-smoke.log` plus
`settings/*.png`. Managed restart evidence is `managed-restart2.log`,
`managed-game.png` and `managed-restarted.png`. Scratch contains derived/game
material and must not be tracked.

Before release, diagnose the normal race stall and nonfinite audio, repeat
the race through a complete lap and exit/save/reload, and verify cold boots.
01.04 regression play, the separate renderer-error validation, matching PGO
builds and a final dual-engine player-ZIP smoke remain required.
