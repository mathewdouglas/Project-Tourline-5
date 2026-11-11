# GT5: toward 60 FPS at the car list and in races

Written for GT5 Phase 44 (2026-11-11), target **BCUS98114-v104**. The
measurements behind it are in [progress-gt5.md](progress-gt5.md), Phase 44.
Anything not marked measured is a hypothesis.

## Where a car-list frame goes (measured)

720p car list, `PS3_TIMELINE` from Phase 44's evidence run (177 frames, mean
22.5 ms). One cycle, from the game thread's (`gFKsE`) flip:

| Step | Time | Notes |
| --- | --- | --- |
| Game thread works, kicks wid 0/1/2/5/6 | ~8.4 ms CPU | runs in parallel with wid4 |
| **wid4 pass** (4 lanes, body `0x0196FF40`) | 20.2 ms mean | kicked right after the flip; the critical path |
| Last FIFO walk after wid4 ends | ~1.4 ms | the frame's tail commands |
| User command raised -> delivered | 0.65 ms mean, 1.33 p90 | delivered at the next syscall of any guest thread |
| Game thread wakes, flips, kicks wid4 | ~0.1 ms | |

- The four lanes started 0, 1.5, 2.9 and 3.9 ms after dispatch
  (`SPURS_LANE_STAGGER_MS=1`: lane k sleeps k ms) and ended together; with
  an even start the same lane work fits in ~2.1 ms less (60 passes).
  **Measured in Phase 44: +9% car-list frames without the stagger.**
- Lanes are CPU-bound during a pass (72-91% of a core each). The ring-space
  wait in the body (LS 0x4540-0x4600: DMA the output ring's read label, compare
  with the write window, delay 2,400 decrementer reads) was 0.1% of the lane
  samples: at the car list the FIFO walker keeps up (walks of 5-70 us every
  ~50 us during the pass); it is not what the lanes wait on.
- Lane time: 59% the render body's own code (0xB240 19%, 0xAD30 7.5%,
  0xF970 6.7%, 0xC5E0 4.9%, ...), 41% runtime (DMA 7.9%, indirect branch 7.0%,
  lookup 4.6%, drain 3.9%, interrupt restore 2.6%, walker wake 1.6%).
  The lifted body costs about 20 x86 instructions per SPU instruction in its
  hottest function (shufb emulation, float ops through doubles); the memory
  traffic of keeping registers in `ctx->gpr` is not the main cost.

## Races (measured by the other thread and here)

High Speed Ring, 50 s frame tests: ~52 FPS whatever is done to wid4 (FMA
copy, lane pinning, stagger). A 4 s timeline 125 s into a run (`rtl1`) ran at
60 FPS: game thread 9.3 ms CPU a frame (56% busy), walk 7.0 ms/frame, wid4
passes ~5 ms. The ~52 FPS average comes from slower stretches of the lap
(30% of intervals over 20 ms), which this window missed.

## Changes in Phase 44 and their status

| Change | Switch | Default | Result |
| --- | --- | --- | --- |
| No lane start stagger for GT5 | `SPURS_LANE_STAGGER_MS` (GT5 default 0; `=1` old) | on for GT5 | car list +9%; race unchanged |
| Register promotion of 14 hot body functions | `GT5P_SPU_REGPROMO` (0/1/2 check/3 A/B) | off | **slower** (lane CPU +14%); exact |
| Indirect returns skip the full drain | `SPU_DRAIN_RET_FAST` | on | no measurable change; exact |
| Sleeping thread delivers user commands | `PS3_USLEEP_GCM_WAKE=1` | off | no measurable change |

## Next steps, in order

1. **Race timeline of a slow stretch**: pick the window from a frame test's
   `frames.csv` (the late intervals cluster by lap position) and place
   `PS3_TIMELINE` there. Does the frame wait on wid4, the walk, or the game
   thread? Only then pick between:
   - the FIFO walk (8.5 ms/frame of draw recording on one thread; Prologue's
     Phase 105/108 cuts are already in) - moving recording off the walker or
     recording fewer state changes;
   - the game thread's own CPU (Phase 30: spread thin; no single hot spot).
2. **Body code cost** (hypothesis): `spu_shufb` and the float helpers are the
   bulk of 0xB240. Constant shuffle masks (most `shufb` controls are loop
   invariants loaded once) could be specialised at lift time into fixed
   `pshufb` patterns; the float helpers' double round trip could use FMA-free
   single precision where the SPU rounding mode allows it. Both change code
   that every SPU image shares: measure with `SPU_EXP_AB` first.
3. **Interrupt return cost** (hypothesis): `spu_irq_regs_maybe_restore`,
   `spu_restart_dispatch` and `spu_halt` samples are all inside the CRT:
   `longjmp` unwinds every host frame on Windows x64. A non-unwinding jump
   (clang's `__builtin_setjmp`/`__builtin_longjmp`) could save up to ~3% of
   lane time. Count first with `SPU_IRQSTAT=1`.
4. **Combine** the car-list wins: no stagger + `GT5P_SPU_FMA=1` +
   `SPURS_LANE_CPU_WIDS=4` were each ~+8-10% alone at the car list; the
   last two did not help races, so a combination would be car-list-scoped.

## How to measure

- One `gt5.exe` at a time; `scratch\go.ps1 -Tag t [-Route race -FrameTest]
  NAME=VALUE` wraps `scripts\test\gt5boot.ps1` and refuses to start while one
  runs (it is in the ignored scratch folder of the Phase 44 worktree).
- Car list: steady windows of `[present-thread] 5 s: N frames` after the list
  appears (300 = 60 FPS). Boot-to-boot spread is ~+-3%; use 2+ interleaved runs.
- Within one boot: `SPU_EXP_AB=1 SPURS_LANE_CYC=1` flips `g_spu_exp` every
  5 s; `[lane-cyc] ... exp=` lines give wall and CPU per lane run.
- Race: `-Route race -FrameTest -Settle 50`, 2+ interleaved runs, medians.
