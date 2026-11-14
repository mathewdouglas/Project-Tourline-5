# SPU code generation review (2026-11-14)

## Decision

Enable **only the integer SIMD helpers** in the normal build. Rollback:
`cmake -S . -B build -DGT5P_SPU_CODEGEN_HELPERS=OFF`, then rebuild.
They cover integer bitwise operations, scalar-store control masks and shifts.
Floating-point policy, channels, DMA and the measured Eiger merged regions
are unchanged. Portable and FMA SPU sources receive the same flag; no new
SPU lift is needed. The isolated runtime and cumulative patch include them.

Whole-image lazy locals, System V calls, cached LS accessors and shuffle OR
remain private experiments. The combined package regressed here. The cache
stays at 16 entries, which already fit the measured traffic. Job-context reuse,
direct LS staging and the main two-shuffle `shufb` improvement already existed.

References: MormonFTP-hub/gt5-recomp
[`e363879564`](https://github.com/MormonFTP-hub/gt5-recomp/commit/e363879564)
and [`1590f03821`](https://github.com/MormonFTP-hub/gt5-recomp/commit/1590f03821).
MIT attribution is retained in `LICENSE`. Its reported 20% CPU/7% FPS gains
are not measurements of this port.

## Live measurements

Previously protected tests were left alone; live work began after the user
released the slot. Sequential games, no compiler running, non-PGO Release,
720p, 2x MSAA, DXC, VSync off, warm shader caches and matching routes.
Existing Eiger regions remained enabled. Audio nonfinite scrubbing was off.
Race samples lasted 60 seconds after a 20-second delay; car list used 40
seconds. Each value is the median of **two runs**, not a confidence interval.
CPU/frame aggregates process CPU time across all threads.

| Scene | Baseline FPS | Helpers FPS | Change | CPU/frame baseline → helpers | Late frames (>20 ms) |
|---|---:|---:|---:|---:|---:|
| High Speed Ring | 54.07 | 56.05 | +3.7% | 119.52 → 112.25 ms | 21.12% → 13.92% |
| Eiger rain | 51.67 | 54.56 | +5.6% | 107.42 → 101.12 ms | 31.95% → 18.61% |
| Car list | 49.13 | 50.30 | +2.4% | 114.23 → 110.58 ms | 55.95% → 44.11% |

HSR order was B/H/OR/L/S/C/C/S/L/OR/H/B (baseline, helpers, shuffle OR,
lazy locals, System V, locals+System V). Eiger and car list each used B/H/H/B.
Both HSR and Eiger helper runs exceeded both baselines; **car-list runs
overlap**, so its smaller gain is less certain. HSR p99 improved 34.41 →
31.82 ms and car list 25.47 → 24.57 ms. **Eiger p99 worsened slightly,
26.32 → 27.09 ms**, despite fewer late frames. These are short-route checks,
not a completed-race, long-session or full-game validation.

| HSR variant | FPS | Process CPU/frame | SPURS job thread CPU (cores) |
|---|---:|---:|---:|
| Baseline, 16 slots | 54.07 | 119.52 ms | 0.694 |
| Integer helpers | 56.05 | 112.25 ms | 0.629 |
| Shuffle final OR | 55.13 | 116.70 ms | 0.688 |
| Lazy locals | 52.40 | 124.01 ms | 0.767 |
| System V only | 53.70 | 117.14 ms | 0.674 |
| Locals + System V | 51.36 | 124.65 ms | 0.744 |

The initial lazy prototype excluded 14 legacy promotion functions and the
measured region members. A fuller follow-up localized those 14 functions too,
cached/reloaded LS base across calls and included System V, helpers and OR.
Eiger regions and all five audio rounding scopes remained intact. A fresh HSR
B/F/F/B gave **54.51 baseline versus 52.82 FPS full package**, CPU/frame
118.16 → 121.82 ms and late frames 19.26% → 27.02%. It remains disabled.
This tests the proposed strategies, not all upstream float/AVX2 policies.
Shuffle OR alone is less convincing than the helper group and stays optional.

Raw plans/results are `scratch/spu-codegen/hsr-plan.json`, `hsr-results.json`,
`followup-plan.json`, `followup-results.json`. Each run has a SHA/settings
manifest under `scratch/spu-codegen/live/`, with frames, named-thread CPU,
logs and screenshots under `scratch/pv/`. `spu_live_summary.py --html` produces
an interactive chart. No private game data is tracked.

## Correctness and cache evidence

- The diagnostic menu/car-list/HSR trace contains 288,128 rows and 14 unique
  image keys. Replaying the original shared topology gives **14 misses and
  447,552 hashed bytes** at every capacity from 16 through 128. Eight slots
  churned (79,615 misses). A per-worker alternative also showed no benefit
  above 16 (55 misses, 1,895,072 bytes). Neither topology nor capacity changes
  were adopted. Full-hash verification reported zero stale sparse matches
  on this route; this does not prove every job image fits or is immutable.
- Adopted helpers passed **3,800,000 exact comparisons**, random vectors and
  all encoded immediate counts, against original callable helpers. Shuffle
  variants passed 200,000 cases each; experimental cached-LS accessors passed
  100,000. Fingerprint tests cover mutations, wraparound, sparse blind spots,
  full verification and four concurrent workers.
- Sixteen captured Eiger loop states and sixteen captured car-list block
  states matched GPR/LS across compared variants. The final car-list harness
  also hashes PC/interrupt state and stops at the first external transfer.
  It does not assume the hot block returns independently. Car-list replay
  median was 330.8 us original, 281.6 us helpers and 386.5 us full package;
  these are block microbenchmarks, not whole-job or FPS predictions.
- All 52 portable images and eight FMA aliases compiled for the measured
  variants. The private reference lift matched normal postpatched sources
  exactly. Native hooks, promotion rollback copies, Eiger regions and five
  audio round-toward-zero call scopes were preserved.
- Experimental parity compares registers, LS, PC, interrupt state,
  image/status/depth and context-mutating call doubles. Synthetic loops,
  branches, joins and a 100,000-link tail chain exercise boundaries.
  Initial counts: Eiger 16,064 states; car list 7,072; mixer FMA 7,344;
  DSP FMA 4,160. Full-local/cached-LS checks added car list 7,104, mixer 7,376
  and DSP 4,176. Experimental car-list locals have **17 paired NaN sign/payload
  differences**, reported separately, so this is not an exact arbitrary-state
  pass. Other completed block checks had zero differences. Adopted helper-only
  comparisons are exact. No exhaustive job/DMA equivalence is claimed.
- Private runs reached boot/menu, car list, HSR, rainy Eiger and opening movie;
  images were inspected. Logged PCM was finite and nonzero with scrubbing off.
  Combined HSR PCM contained 14,158,848 finite samples (last 20 seconds RMS
  0.2555). This does not claim subjective listening quality. Startup microstep
  reports also present in baseline were not treated as new regressions.

## Integration and reproduction

Only `runtime/spu/spu_codegen_helpers.h` and its include were installed into
`../ps3recomp-tourline5`. Experimental cache, lifter, ABI and LS changes were
not installed. Generated sources are unchanged. CMake helper default is ON;
`GT5P_SPU_SHUFB_OR` defaults OFF and requires the private runtime. The cumulative
patch was refreshed from raw Git bytes with a temporary index, preserving
existing compatibility edits. Pinned clean application reproduces installed
sources; `setup_runtime.py --check` passes.

```powershell
.venv/Scripts/python scripts/test/spu_codegen_runtime.py --runtime ../ps3recomp-tourline5 --helpers-only
.venv/Scripts/python scripts/test/audio_extended.py
.venv/Scripts/python scripts/test/audio_coefficients.py
.venv/Scripts/python scripts/test/audio_math.py
cmake -S . -B build -DGT5P_SPU_CODEGEN_HELPERS=ON
cmake --build build --parallel 2
```

Regenerate profiles before making a PGO release from changed sources. These
measurements use matching non-PGO builds. No player package or release is made.

The normal Release rebuild passed (all 52 images and eight FMA copies), as
did the installed helper and audio extended/coefficient/fused-arithmetic
regressions. `spu-production-accept` then reached HSR in `build/gt5.exe`,
completed its 40-second sample and exited through the runner. The inspected
race image shows road, cars, HUD and rear-view; audio stayed finite/nonzero
with scrubbing off. All 24 timed comparison logs were also scanned with no
nonfinite audio, watchdog, fatal error or assertion failure found. The normal
smoke is acceptance evidence, not an extra controlled performance comparison.
Fresh preparation reproduces all optional candidate sources (allowing Git's
checkout CRLF conversion).

The optional `patches/incremental/gt5-spu-codegen-experiment.patch` is relative
to the adopted runtime; setup does not apply it. Preparation copies installed
sources and refuses existing snapshots. All guest-derived data stays ignored.
Use a new `--output` directory to verify preparation without overwriting the
original measured snapshots. Lift/compile drivers run idle priority with one
compiler; do not compile during timed games.

```powershell
.venv/Scripts/python scripts/diag/prepare_spu_experiment.py
.venv/Scripts/python scripts/test/spu_codegen_runtime.py --bench
.venv/Scripts/python scripts/diag/lift_spu_experiment.py --name reference
.venv/Scripts/python scripts/diag/lift_spu_experiment.py --name lazy2 --lazy
.venv/Scripts/python scripts/spu_sysv.py --input scratch/spu-codegen/reference/generated/spu_lifted --output scratch/spu-codegen/sysv/generated/spu_lifted
.venv/Scripts/python scripts/spu_sysv.py --input scratch/spu-codegen/lazy2/generated/spu_lifted --output scratch/spu-codegen/lazy-sysv/generated/spu_lifted
.venv/Scripts/python scripts/diag/lift_spu_experiment.py --name lazy-ls --lazy --cache-ls --full-locals
.venv/Scripts/python scripts/spu_sysv.py --input scratch/spu-codegen/lazy-ls/generated/spu_lifted --output scratch/spu-codegen/full-reference/generated/spu_lifted
.venv/Scripts/python scripts/test/spu_codegen_compile.py --variant full-reference --helpers --shuffle-or
.venv/Scripts/python scripts/diag/link_spu_experiment.py --variant full-reference
.venv/Scripts/python scripts/diag/link_spu_experiment.py --cache-only
```

Private linking reuses read-only PPU/host objects and a copied runtime archive,
replacing workload dispatch. Stored pointers keep Windows wrappers; direct and
tail calls use TU-local System V bodies, preserving `musttail` and portable/FMA
symbol isolation. `SPU_NO_SYSV` disables the ABI attribute. Lazy output requires
`SPU_LS_NO_PROBES`; register-reading diagnostics must use original lifts.
Cached LS base reloads after context-mutating calls. For a separate CMake
build, override `PS3RECOMP_DIR` and `GT5P_SPU_GENERATED_DIR` in a private build
directory. `spu_codegen_parity.py` accepts `--image`, `--sysv`, `--fma`, and
`--new-root ... --cache-ls` for the fuller prototype.

Live runs use `scripts/test/spu_live.ps1` or declared `spu_live_suite.py` plans.
The runner waits for other games and stops only its own child. Trace/full-hash
verification runs perturb performance and are excluded from timed tables.

Measured private executable SHA256 identities:

| Variant | SHA256 |
|---|---|
| Baseline/cache-only | `2eba75948ce30158fdf5d8af4f321fc33211d09bb12ab797e6dfc817001960dd` |
| Helpers | `65220cf8db2f6436ae2b31a2afc5658d2c7ee2b71ccfefb1031106980096467d` |
| Shuffle OR | `27f7a17d355b4cf342bbfa45541585acf4e66b80db9c42c042fbe80540e7cadd` |
| System V | `e56ac0c19f04b98dbbd884c4551604988faabfdbae4f785ffae3df714e5c1fc8` |
| Conservative locals + System V | `b7c9cf9c9c9f80190701257ff45f9874f102bcdaafd44ae3db59795b2dfc7da9` |
| Full package | `a5554f1be51fd40c4e225a8d1d86ac090455f7cd979582fe087d63af683c37a8` |
