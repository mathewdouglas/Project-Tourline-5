# Profile-guided builds (PGO)

A PGO build compiles `gt5.exe` twice. The first build is instrumented: it
counts every branch. One run of the game writes those counts to a profile. The
second build reads the profile and lays out code by how it actually runs:

- hot code is packed together;
- branches are ordered the way they usually go;
- inlining goes where the calls are;
- code that never ran is compiled for size.

This covers the lifted PPU code, the lifted SPU code and the runtime alike,
since all three compile through the same compiler launcher.

The approach follows Soulbrandt's `tools/pgo` and its docs/58 (MIT,
JonathanDC64/Soulbrandt). The scripts here are written for this build.

## Commands

```
.venv\Scripts\python scripts\pgo\pgo.py gen          # build_pgo_gen\gt5.exe, instrumented (2 jobs)
.venv\Scripts\python scripts\pgo\pgo.py play         # High Speed Ring race route; counts -> scratch\pgo\profraw
.venv\Scripts\python scripts\pgo\pgo.py merge        # -> scratch\pgo\gt5.profdata
.venv\Scripts\python scripts\pgo\pgo.py use          # build_pgo_use\gt5.exe, optimised (4 jobs)
.venv\Scripts\python scripts\pgo\pgo.py ab           # alternating frame tests: build\ vs build_pgo_use\
.venv\Scripts\python scripts\pgo\pgo.py clean-gen    # delete build_pgo_gen\ once merged
```

| Step | Time here (i5-12400F, 16 GB) | Notes |
|---|---|---|
| `gen` | about 20 min at 2 jobs | Configured like `build\` (target, runtime, `GT5P_*` options). The exe is 394 MB. Four jobs risks running out of memory on 16 GB. |
| `play` | about 3 min | Runs `gt5boot.ps1 -Route race -FrameTest` (boot, menus, car list, loading, 60 s of racing). The instrumented game races at about 17 FPS. 54 MB of counts. |
| `merge` | seconds | 59 MB profile. |
| `use` | about 12 min at 4 jobs | Prints how many functions no longer match the profile (0 when the sources and lift are the ones it was made from). |
| `ab` | about 25 min for 4 rounds | Rebuilds `build\` first so both sides have the same sources. Each round runs both builds, and the order flips every round. |

- **Paths**: `build_pgo_*\` are ignored (`build_*/`); the profile lives in
  `scratch\pgo\`. It is made by running the game, so it is derived data like
  the lift and is never committed.
- **Running it**: `gt5boot.ps1 -Exe <path>` runs any build;
  `build_pgo_use\gt5.exe` also runs directly like `build\gt5.exe`.

## How it works

- **Compiler launcher**: `scripts\pgo\pgocc.py` is set as
  `CMAKE_C_COMPILER_LAUNCHER` and `CMAKE_CXX_COMPILER_LAUNCHER` of the PGO build
  folders only. It puts the profile flags into every compile command, so a new
  profile path recompiles everything. CMake's own try_compile checks run
  without them.
- **IR instrumentation**: `-fprofile-generate` / `-fprofile-use`, with
  `-static-func-full-module-prefix=false` so a profile fits a checkout in
  another folder.
- **Continuous mode**: `LLVM_PROFILE_FILE=...gt5-%p%c.profraw`, with
  `-runtime-counter-relocation` and value profiling off. Counts are written
  while the game runs, so the test script's kill loses nothing.
- **The instrumented link**: clang's `clang_rt.profile-x86_64.lib` is built
  `/MT`, and the game is `/MD`. `gen` passes `/LIBPATH` and
  `/NODEFAULTLIB:libcmt.lib` through lld-link's `_LINK_`. Soulbrandt also
  needed a `getenv` thunk; this build does not, because it does not override
  `__imp_getenv`.
- **Staleness**: a profile fits the sources and lift it was made from.
  Changed functions lose their counts (clang's "hash mismatch" warning), and
  everything else keeps its counts. Re-run `gen`, `play` and `merge` after a
  re-lift or larger source changes.

## Measured (2026-11-12)

Same sources (main 6066fe8 plus these scripts), same runtime (main's patch),
same lift. The profile came from one High Speed Ring race route.

**High Speed Ring frame test** (`pgo.py ab`, 4 rounds, 40 s windows):

| | normal | PGO | PGO - normal | rounds PGO better |
|---|---|---|---|---|
| FPS | 53.45 | 56.65 | +3.20 (+6.0%) | 4 of 4 |
| late (>20 ms) | 24.1% | 13.2% | -10.9 points (-45%) | 4 of 4 |
| p99 interval | 32.4 ms | 32.8 ms | +0.4 ms | 1 of 4 |
| FIFO walk | 8.79 ms | 7.54 ms | -1.26 ms (-14%) | 4 of 4 |

Every PGO run (56.5-56.8 FPS) was above every normal run (53.1-54.0). This
machine's usual spread between runs is about 5 FPS, but within this
interleaved set each side varied by under 1 FPS. The picture and race state
match (same HUD and speed, lap timer within 0.3 s). Both builds logged the
same 16 SPU microstep lines and no faults.

**Arcade car list**, out of sample (the profile only passed through it):
alternating A, B, B, A, mean of the last 35 s:

| Run | FPS |
|---|---|
| normal A1 | 49.2 |
| PGO B2 | 51.0 |
| PGO B3 | 50.9 |
| normal A4 | 50.1 |

That is about +2.5%; the car list is held by the SPU job chain (workload 4).

**Not measured**: other tracks, Eiger in rain, menus beyond the route, and a
profile merged from several routes.

## Releases

`scripts\release\clean_build.py --profile <profdata>` builds the player ZIP
profile-guided. The profile is one more private input, like the ELF: the clean
clone's own `scripts\pgo\pgocc.py` compiles everything with it, and
`release.ini` in the ZIP records its SHA-256 (`pgo_profile_sha256`).

Make the profile from the release's sources:

1. Check out the release tag in a development checkout. Its `generated\` must
   be the tag's lift with every post-lift step applied. `pgo.py gen` and
   `use` refuse a lift that misses one.
2. `pgo.py gen`, `play`, `merge` (about 25 minutes).
3. `clean_build.py --private <checkout> --work <new folder> --profile <checkout>\scratch\pgo\gt5.profdata`.

`clean_build.py` reads clang's per-function warnings from the build log
(`build.log` in the work folder):

| Count | Meaning | Limit |
|---|---|---|
| lost counts (hash mismatch) | a function that ran while profiling has a different body now | 0 (`--profile-max-stale`) |
| no record | code the profiling run never loaded: the launcher (another exe), the settings page, debug export, glyph art | 2000 (`--profile-max-missing`) |

A profile that fits these sources gives no lost counts and about 500 functions
with no record. A profile from other sources misses tens of thousands.

**First test (2026-11-13)**: the profile behind the measurements above failed
this check with 25 functions that lost counts. They were the hot loops of
bodies `b01697100` and `b0196FF40`: this checkout's `generated\` had never had
`spu_regpromo.py` and `spu_region.py` applied, and the release lift applies
them. Both sides of the A/B used the same lift, so the High Speed Ring result
stands. `spu_region` targets an Eiger loop that does not run there, and the
promoted copies are off by default. After the two steps were applied, the dev
lift was byte-identical to the release lift, and the profile was made again.
The release test then passed (0 lost counts, 435 without a record), and its
packaged `gt5.exe` ran 55.1 and 55.7 FPS at High Speed Ring against 54.9 for
the earlier PGO build in the same session.

## Open

- **Release cadence**: re-profile for every release. Code changes since the
  profile lose their counts, and the release check refuses them.
- **A/B measurements**: a change measured on PGO builds needs a profile per
  side, since changed code has no counts in the other side's profile. Keep
  A/Bs on normal builds unless both sides are profiled.
- **Profile coverage**: a route through Eiger and the car list would give
  those scenes their own counts.
