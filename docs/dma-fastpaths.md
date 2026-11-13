# SPU DMA fast paths

Implemented on 2026-11-13, using Ascension-R `2c5219888fcaeea1dd0c2ed3d0895bf0023df138`
and its runtime `88ac02a4de74ad310afbe186ecb400d8527e785b` as references.
The reference code is MIT licensed; its attribution is retained in `LICENSE`.

The change is in `patches/ps3recomp-5244db9-local.patch`. The review copy is
`patches/incremental/gt5-dma-fastpaths.patch`; setup does not read that copy.

## Behavior

- Twenty-eight optional diagnostic blocks share one cache-line-aligned gate
  instead of reading their separate inactive switches on each transfer.
  All 27 enabling variables retain their existing parsing. GT5's job-descriptor,
  spin, PUT-value and range-history probes are included.
- Writes to LSA, EAH, EAL, size, tag and tag mask latch the same context fields
  without allocating or looking up an MFC engine. Tag IDs still mask to five bits.
- MFC reads use the existing context-only reader without an engine lookup.
  The list-stall read still clears its status. Commands, tag updates and list
  acknowledgements retain their existing engine paths and event handling.

`SPU_DMA_FAST=0` restores the original diagnostic dispatch and engine lookups.
`SPU_DMA_DIAG=1` forces the original diagnostic checks while retaining the
channel fast paths. Set these before launch; the options are cached at first use.
The new option caches use atomic publication for concurrent SPU workers.

Transfer validation, memory commits, copies, overlay tracking, lock-line
coherency, hooks, the repeated-transfer watchdog and existing default-on
diagnostics are unchanged. No Ascension addresses, job replacements, lock
striping, interrupt-stub cache or approximate arithmetic are introduced.

## Isolated validation

```
python scripts/test/dma_fastpaths.py
```

The test materializes the pinned runtime and cumulative patch under a fresh
`scratch/dma-fast-test-*` directory with a private Git index. Compiler and test
processes run at idle priority. It does not change the active runtime, build,
generated code, caches, settings or game processes.

Native checks compare the production header with fast paths on, rollback and
forced diagnostic dispatch. They cover 5,000 register writes, 300 transfers,
tag updates, invalid-transfer rejection, reserved/unreserved PUT hook counts,
list stall/read-and-clear/resume and atomic status reads. Selected diagnostic
output must match too. Every gate variable is checked independently, including
the value `0`. The entire production `spu_channels.c` also compiles.

During implementation, the same checks were compared with the exact header
from before this change. Removing only the added helpers and gates reproduced
that header exactly. State digest: `F6FDF543`; hooks: 152 overlay calls,
150 PUT calls, 150 guard checks, 75 lock acquisitions, 1,772 notifications and
one list wake. Host hooks are deterministic test doubles; this is not a
multithreaded coherency stress test or proof of in-game performance.

The initial implementation left the active runtime unchanged while Opus was
running tests. The subsequent live validation below integrated only the DMA
incremental patch, preserving the runtime's other edits and pending audio work.

## Live acceptance and performance (2026-11-13)

Built `build/gt5.exe`, the existing non-PGO Release target, with clang-cl and
the isolated tourline5 runtime. No re-lift or shader changes. Executable SHA-256:
`3F8792B11F39502BA8C0BB0CB26BEDB9BFC1C2EEEF62A0618E18CFC48AA3AC28`.
Machine: i5-12400F, RTX 4060. All comparisons used this same executable:

- `PS3_VERBOSE=0`, 720p, 2x MSAA, `RSX_PRESENT_SYNC=0`;
- DXC 1.8.2502/SM 6.0, startup shader warmup on, existing cache retained;
- built-in settings, 20 seconds after the target milestone before sampling;
- throttle held in races, no steering; 60-second race and 40-second car-list
  samples, with game-window captures every 20 seconds;
- one `gt5.exe` at a time. The audio thread waited until these runs finished.

High Speed Ring used **on/off/off/on** order, with a separate initial warmup
excluded. Eiger used a separate warmup, then one off/on pair. The car list used
one off/on pair after all the race runs had already visited it. Secondary
pairs are acceptance checks with preliminary performance observations, not
repeatability measurements.

| Scene | Off FPS runs | On FPS runs | Off -> on median | Change |
| --- | --- | --- | --- | --- |
| High Speed Ring | 51.088, 53.016 | 53.256, 53.251 | 52.052 -> 53.254 | +2.3% |
| Eiger Nordwand, rain | 48.427 | 50.925 | 48.427 -> 50.925 | +5.2% |
| Car list | 48.853 | 48.939 | 48.853 -> 48.939 | +0.2% |

| Scene | Late frames >20 ms, off -> on | p99 ms, off -> on | FIFO walk ms/frame, off -> on |
| --- | --- | --- | --- |
| High Speed Ring | 30.1% -> 24.1% | 34.23 -> 34.77 | 8.02 -> 8.30 |
| Eiger rain | 50.7% -> 36.5% | 33.75 -> 26.93 | 6.31 -> 6.14 |
| Car list | 58.3% -> 58.3% | 24.97 -> 25.60 | 6.42 -> 6.53 |

HSR's enabled runs were consistent and both exceeded both rollback runs, but
the rollback spread was 1.93 FPS. The 1.20 FPS median difference is smaller
than that spread; this supports a modest preliminary result, not a precise
repeatable gain. HSR p99 did not improve. Eiger's one-pair difference is inside
the roughly 5 FPS spread documented in Phase 45. The car-list result is neutral.
Keep the fast paths enabled, retaining `SPU_DMA_FAST=0` for rollback; do not
describe this as a general 5% improvement or a fix for the Eiger serial loop.

Installed boot/menu, car-list presentation, High Speed Ring racing, Eiger
rain racing and 60 seconds of the opening movie passed the visual smoke
checks. Captures show the track, cars, HUD and rear-view, the car and floor
reflection, and changing movie frames. No crash, GPU-device-loss or stall
reports appeared. Existing image-15 microstep/interpreter fallbacks appear
in both modes. These are short checks, not full races, a complete playthrough,
audio acceptance or validation of every resolution.

Reproduction:

```
powershell -NoProfile -File scripts/test/dma_live.ps1 -Case HSR -Order 1,0,0,1
powershell -NoProfile -File scripts/test/dma_live.ps1 -Case Eiger -Tag dma-warm -Order 1 -Settle 40
powershell -NoProfile -File scripts/test/dma_live.ps1 -Case Eiger -Order 0,1
powershell -NoProfile -File scripts/test/dma_live.ps1 -Case CarList -Order 0,1 -Settle 40
powershell -NoProfile -File scripts/test/dma_live.ps1 -Case Movie -Order 1
```

`gt5boot.ps1` now waits instead of killing another game, stops only its own
child and writes precise sample-window/metric JSON alongside the existing
text and CSV. Raw evidence is ignored under `scratch/pv/dma-*`; summaries,
build log and chart are under `scratch/dma-live/`. No shader cache was moved
or deleted, and neither shaderstamp checkout was accessed.
