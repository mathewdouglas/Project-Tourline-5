# GT5 2.11 live checks (2026-11-15)

The 0.1.6 candidate is not ready for release. The normal 2.11 race route
reaches playable High Speed Ring, but produces invalid audio and subsequently
stops presenting. These are bring-up failures, not a completed race pass.
Both are now fixed: the audio NaNs (see `docs/audio.md`) and the freeze (see
"Race freeze fixed" below). The startup stall is separate and still open.

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

## Race freeze fixed: per-thread TLS (2026-11-15, fifth pass)

Cause: every guest thread made by `sys_ppu_thread_create` shared one TLS
block. The runtime only learns the TLS template from `ppu_load_elf`; this
port loads the ELF itself (`src/elf_loader.h`), so the template stayed empty
and `ppu_tls_new_block()` gave every created thread `r13 = 0x10F07000`. Only
the main thread had its own block (from `sys_initialize_tls`, `0x0E007000`).

GT5 2.11 keeps the thread's current render list in TLS (`r13-0x6FF8`, set by
`func_008F3A8C`, read by `func_008F32AC`, finished by `func_008F34B4`). Its
two race render threads (guest ids 30 and 32) began their own lists
(`0x4FFEE090`, `0x4FFEE0D8`), but thread 30 then finished thread 32's list,
under the list lock but while thread 32 was still appending outside it. The
terminator was written mid-record and the write pointer moved backward
(`PTR-BACKWARD` in the reserve trace), so job body `0x015DCC00` read a
malformed list: the loop at LS `0x4840` or a bad command block, then the
walker's 3 s resync and the freeze in the passes below.

Fix (runtime, `patches/ps3recomp-5244db9-local.patch`):

- `sys_initialize_tls` passes the template to `ppu_tls_set_template()`, so
  created threads get their own copy (`PS3_TLS_FROM_CRT=0`: old shared block).
- Thread blocks are packed by the template size (0x1FC, so 4 KB each) instead
  of 32 KB. The 1 MB window held 31 blocks at 32 KB; thread 30 got the 36th
  (`PS3_TLS_PACK=0`: 32 KB). A full window now prints `[ppu] TLS window full`.

Measured: with the fix, threads 30 and 32 run at `r13 = 0x10F2B000` and
`0x10F2D000`, each finishes only its own list, and no backward move is
logged. Three 230 s High Speed Ring races (one instrumented, two clean)
reached lap 2 (2:09-2:13 race time) with no stall; before it, every race run
today froze at about 85-120 s. A 01.04 race on the same change also ran to
the end of the window (1:50 race time, about 60 FPS).

This was not a 01.04 fix missing from 2.11: 01.04 shares the same TLS block
across threads in every earlier build. Why 01.04 never froze from it is not
measured (its list code may not keep the current list in TLS from two
threads).

## Follow-up diagnosis (2026-11-15, later)

Same route, settings and private 2.11 root (the launcher-test copy under
`scratch/release016-live/home/targets/BCUS98114-v211`), one game at a time.
Eight runs. Every run reached the race and froze between about 85 and 120
seconds. Neither failure is fixed yet.

**Spin waits (kept).** v104's five `spin_waits` were mapped to 2.11 by
masked byte matching (call targets and TOC offsets ignored). Two loops match
uniquely: `0x00B99648` (loop `0x00B996C8`) and `0x00B9BB90` (loop `0x00B9BBB8`).
v104's three single sites (`0x00C34BDC`, `0x00C35254`, `0x00C357FC`) have no
2.11 counterpart: 2.11 polls the same check (`func_00AA8528`) from
`func_00AA85F0`/`func_00AA86C0`, sleeping 100 us with `sys_timer_usleep`
between tries. They need no patch. `scripts/test/spin_waits.py` now takes
either target and passes for 2.11 (one read-only back edge each, exact
patch/restore). This is v104 parity work; it does not fix the freeze, and its
2.11 performance effect is not measured.

**Race freeze: one input list measured, more than one trigger.** Each
frozen run follows the same sequence. A render SPU job does not finish its
command block. The FIFO walker parks on that block, and after 3 s it resyncs
past it (`jump-park-timeout`). The guest's pollers (`func_00AA85F0`) then
never see their progress marker again, and nothing is presented. The resync
turns a late or broken job into a permanent freeze.

In four of the eight runs the broken job was body `0x015DCC00`. A one-off
hook (private; `scratch/v211/b015DCC00-listcheck.diff`, removed from
`generated/` afterwards) showed:

- The job walks a PPU-built record list. Each record has a type byte and a
  size byte at bits 16-23, and the list ends with a zero word. The list
  arrives by DMA from guest `0x4FF8E300`, with the size from the job's
  descriptor. PPU writers: `func_00831DD8` (reserve a record against a limit)
  and the per-type helpers at `0x00871xxx`.
- In 6,400 good jobs the terminator is exactly the last 4 bytes of the
  DMA'd size. In every one the guest buffer was already reset (zero at `+0`)
  when the job body began. The input is fetched when the job is loaded, and
  the PPU reuses the buffer straight after.
- Bad jobs (1 in about 800-1,100) get a list that does not end inside the
  DMA'd size. Twice the data ended exactly at the size and the rest of local
  store still held a previous job body (`0x014FBC00` at its `0x4000` load
  address). The walk then either reaches a zero-size record and spins
  forever at `0x4840` (lane step count frozen; event mask 2), or runs on and
  produces a bad block.
- Hypothesis, not proven: the PPU rebuilds `0x4FF8E300` before or during that
  job's input fetch. The job lists' progress rows (`+0x40 + 16*q`: a take
  counter and per-SPU current-job halfwords, `FFFF` = idle) looked
  consistent for the stuck job in the one row log taken
  (`SPU_ROWLOG`, workload 1, SPU 3 holding job `0x18`). The guard the PPU uses
  before reusing the buffer has not been found.

The other runs froze differently. A workload 4 lane waited on `0x03000480`
while the walker waited on that lane's block. With `SPURS_LANE_PRUNE=0`
(worse: froze at about 85 s), every SPU workload was idle and the FIFO sat on
an unfilled self-jump at `0x00A69080`. No bad list was seen in that run.

**Race audio: fixed later the same day.** The invalid samples are
`0xFFFFFFFF` words from 2.11's EQ image (`socepdideq`), whose state holds
SPU extended-range values that the host treated as NaN. Extended-range
arithmetic for the audio images removes them: 0 invalid-PCM reports in three
races, and they return with `GT5P_AUDIO_EXTENDED_RANGE=0`. See
[audio.md](audio.md), "2.11 race NaNs". The paragraph below is the earlier
reading, kept for the record.

**Race audio: not the extended-zero case.** 2.11 has a new sound image,
`socepdidist` (`j18DE3719`), and a changed `socepdirta` (`jCF635263`).
Neither had the extended-zero fix. A build with the fix on both, and a build
with it on all 15 sound images, still produced the same port-0 NaNs from
race start (`input_nan` 128-768 per block). The round-to-zero scopes in
`j9804ABA8` are present in the 2.11 lift. That experiment was reverted: the
target keeps its measured list. Next step: trace the invalid samples back
stage by stage, as the v104 investigation did ([audio.md](audio.md)).

**Freeze, second pass (same day).** A private runtime check logged every
SPU GET of the job's record list (`scratch/v211/listchk.diff`):

- `0x4FF8E300` in the job's descriptor is not where the list comes from.
  The list arrives in 16 KB chunks from pool buffers such as `0x40152000`
  and `0x40166000`.
- For bad list #2 (size `0xE6F4`), the source buffer held a different,
  shorter list (terminator at `+0x5000`) at the moment of the fetch. The
  job fetched its input after the PPU had begun refilling that buffer for a
  later frame.
- Records are written by guest thread 32 through `func_00831DD8`. Only 35
  PPU stores hit `0x40152000` in 100 s, so the buffers rotate through a
  pool, not a fixed pair.

Two A/B runs:

- `SPURS_WKL_FLAG_KICK=0` stays on the GT logo, so the 2.11 kick is
  required.
- One pipeline-build helper instead of four still froze at about 115 s, with
  two bad lists. Host contention from shader building is not the cause.

Whether the PPU's reuse guard is too weak, or misled by job progress,
remains open.

**Freeze, third pass: the job reads its descriptor while the PPU rewrites
it.** Private diagnostics only. The runtime diffs are
`scratch/v211/fetchlog-*.diff`, logging GETs into LS `0x5C00`, job-queue slot
reads and descriptor reads. They have been removed from the runtime. Findings:

- **The list buffers.** Every frame's record list goes to one of two 80 KB
  buffers, `0x40152000` and `0x40166000`, alternating. A race list is about
  `0xCC50` bytes and is fetched in 16 KB chunks.
- **Not late, not a duplicate.** Each bad job takes the normal job's place
  in the frame rhythm, so it isn't late; no earlier fetch had its size, so it
  isn't a re-run. The size is simply wrong for that frame: for example
  `0x560`, `0x8F0` or `0x228`, while that frame's list is about `0xCC50`.
- **The descriptor block.** The job is described by one descriptor at
  `0x4FF863D0` (inside a job object at `0x4FF863A0`), which the PPU rewrites
  in place every frame.
- **The PPU's write order** (guest thread 32):
  - `func_00AA8888`, via `func_00AAA350`, lays out the descriptor;
  - `func_00AAA084` writes the DMA entries (`+0x10`…`+0x44`, including
    `0x40152000`/`0x40166000` and the list size);
  - `func_00AAA514` copies the job's parameters to `+0x50` through
    `func_00BCD4AC`. That includes the size the job reads at entry: `r6`,
    `{size, 0x4FF8E300, 0x400, 0}`.
  - Each of the three first waits for the previous job on this descriptor,
    through `func_00AA85F0` and the handle at object `+0x2C`.
- **The bad case.** A lane read the descriptor after `AAA084` had written
  that frame's entries but before `+0x50` was updated: new DMA entries, old
  parameters. The queue entry pointing at `0x4FF863D0` appeared in ring slot
  `0x40027180` between those two writes.
- **Not found yet: who appends that entry.** Neither `PPU_WVAL` nor
  `SPU_PUTVAL` caught it; vector stores are invisible to both.
- **Not yet consistent:** in one run `+0x50` was not written every frame. So
  which descriptor field the job takes its fetch size from is still not
  certain.

Rejected this pass:

- `SPURS_LANE_INFLIGHT=1`: no lane ever left with a job in flight, and it
  still froze.
- `GCM_PARK_TIMEOUT_MS=0` (wait for ever instead of resyncing): a bad list
  still hangs the job in its record walk, so the wait never ends.
- Lanes in `Sleep(1)` retry loops (another freeze shape): seen at one stall
  and not pursued.

**Freeze, fourth pass: the record list itself is malformed.** This pass
used timestamped PPU hooks in the lifted code (private copies:
`scratch/v211/*.lag`, `b015DCC00-listcheck3.c`), a ring watch
(`ringwatch.diff`) and lane signal/wake counters. All have been removed.

The third pass's "torn descriptor" reading is wrong:

- On the PPU (thread 32), `func_00AAA084` (DMA entries), then
  `func_00AAA514` (parameters), then the submit `func_00AA9510` all ran about
  0.9 ms before any lane saw the job. The descriptor was complete when read.
- The submit is `func_00AA9510`, called from the frame function
  `func_002BB3E8` after `func_008CA580`.
- The writers' guard (`func_00AA8528`, then `func_00B98828`) never took its
  "3+ frames old, presume done" branch: ages were only 0 and -1. The row
  protocol is conservative: a lane entering from idle takes the job and sets
  its slot in one write (pc `0x3020`).
- No lost lane wake-ups: signals sent equal wake-ups taken for every lane of
  every workload, in two frozen runs.

What the bad jobs do show:

- **The list is broken, and the size matches.** The size parameter is
  written each frame by `func_008F37E8` into `0x4FF86490` and copied into
  the descriptor. In one hang the job's copy matched buffer `0x40166000`
  exactly.
- **Where it breaks.** At `+0xDC0` there's a type `0x0B` record header
  (8 bytes: header plus a float). Where its float belongs, `+0xDC4`, the next
  record's header `0x0E080000` already sits. The size parameter, `0xDC4`,
  ends exactly at that break.
- **Two writers, or a moved pointer.** A single appender advancing 8 bytes
  per record can't produce this. Two appenders on one list, or a write
  pointer reset under the builder, can.
- **In other bad cases** neither buffer held a list as short as the size
  parameter.

**Separate bug: startup stall.** In 2 of about 25 runs today (`lag2`, `ev3`;
logs in `scratch/release016-live/bootstall-*`) the game reached
`installed00.dat` (about 17 s) and never reached `MusicDecode`. The window
stayed at 60 FPS with 8 draws. This is the cold-boot logo stall from the
first checks, not the race freeze.

Next for the freeze:

- find which threads append to the list. Watch the list object's write
  pointer, and the record helpers at `0x00871xxx` and `func_00831DD8`, by
  thread;
- find the code that appends the `0x4FF863D0` entry to the job queue, and
  whether it can run before `func_00AAA514`. Candidates: a vector copy, or
  the render thread's job-list submit;
- find the job input's real size field;
- find who should fill the self-jump at `0x00A69080` (the other freeze
  shape).

Do not shorten or extend the walker's 3 s resync, and do not skip unfilled
blocks to hide the freeze.
01.04 regression play, the separate renderer-error validation, matching PGO
builds and a final dual-engine player-ZIP smoke remain required.
