# Course Maker rendering investigation (2026-11-14)

Status: reproduced after the user authorized live tests and the game test slot
became free. The flicker is fixed in the runtime compatibility patch. Ultrawide
map placement and label clipping remain open. No other game instance was
driven or stopped. Existing caches and saves were retained; the game can update
them normally during a run. Course Maker tests used a private executable in
`scratch/course/`. The normal `build/gt5.exe` was subsequently rebuilt with
this correction during the SPU helper work and passed an HSR smoke check;
Course Maker acceptance itself remains the private runs described here.

## Measured cause and correction

The editor clears only its map panel (`CLEAR_SURFACE` flags `0xF1`, scissor
385,135 506x480 at 16:9/720p). `d3d12_clear` treated every display colour clear
as a completed frame. It presented the title/details before that clear and
then presented the map/settings separately. Eight consecutive 21:9 captures
alternate 194/356/194/356/193/356/193/356 records; the two images contain
complementary parts of the editor. This reproduces the reported flicker after
shader compilation has finished. A 16:9 control also alternates partial frames.

The correction records a scissored display clear in command order and replays
its rectangle at host resolution. A combined depth clear is retained before
that colour clear. It neither presents nor changes the next whole-frame clear
colour. Full-display clears keep the original boundary behavior. Rollback:
`RSX_SCISSORED_CLEAR=0`.

`RSX_ACCUM_FRAME=1` first confirmed the frame-splitting diagnosis, but it drops
the mid-frame clear rather than executing it correctly and is not the fix.
No Course Maker screen/shader addresses or fixed draw-count thresholds enter
the correction.

Live captures/logs (ignored under `scratch/`):

| Run | Configuration | Result |
| --- | --- | --- |
| `course-cap1` | Original, 720p, 21:9 | Alternating complementary editor batches |
| `course-accum` | Original, 21:9, `RSX_ACCUM_FRAME=1` | One ~546-record batch; ultrawide defects remain |
| `course-169` | Original presentation, 16:9 | Same frame split; right labels and map placement correct |
| `course-rect169` | Scissored-clear correction, 16:9 | Title, details, map and right labels present together |
| `course-rect219` | Scissored-clear correction, 21:9 | Flicker removed; map/label defects below remain |

Both corrected runs captured four consecutive editor batches. The native
`scripts/test/scissored_clear.py` compiles the production clear recorder and
replay rectangle arithmetic: combined depth/colour ordering, full-clear
boundaries, preservation of the whole-frame clear colour, offscreen/depth-only
clears, capacity bounds, 720p/ultrawide/4K coordinate scaling and clipping pass.
The rollback reproduces the erroneous mid-frame present. The backend compiles
and links in the private game. The full patch applies cleanly to a private
5244db9 checkout with the runtime's Windows EOL settings; installed-runtime
`setup_runtime.py --check` passes.
Debug-export integration checks also pass, including reporting the new rollback
switch when it is set.

The corrected captures are 541/541/541/541 records at 16:9 and
546/546/546/546 at 21:9. A subsequent 720p/21:9 High Speed Ring smoke
(`course-clear-hsr`, 30 seconds of window sampling from RaceStart) reached
the race and showed the road, cars, HUD and rear-view together. This is a short
visual regression check, not a performance comparison or full race acceptance.
Tested private executable SHA-256:
`bd175221998fb73a45e6e950f7e84699cc7e422058f2d85aadb5421a41060694`.

## Remaining ultrawide defects

At 16:9 the map viewport is 385,135 506x480. At 21:9 its guest rectangle is
289,135 380x480, which widens to approximately the same host position/width.
The surrounding UI gains a centered gutter, but this viewport does not; it
overlays the left details rather than occupying its central panel. The right
labels/values are also clipped away in 21:9 and visible in the 16:9 control.
This establishes an additional aspect-dependent fault; its PPU/UI transform
and label-mask fix have not been measured. Do not copy Prologue hook addresses.
The correction above is a flicker fix, not full ultrawide Course Maker support.

## Report

The supplied screenshot (Screenshot 2026-11-14 105235.png) shows the editor's
contour map, course line, numbered tabs, bottom icons and some panel borders.
Most panel labels/content are absent. The user also reports flicker; a still
image cannot identify which passes alternate. The window reports 1707x720,
96 FPS and 186 draws. These counters do not establish correct presentation
or a completed shader warmup. The executable version/settings are unconfirmed.

## Earlier source lead: texture bounds, not established as this fault

In the isolated runtime's `libs/video/rsx_d3d12_backend.c`,
`off_rt_find_in` (line 9778 at investigation time) finds a containing render
target for a texture address. It computes x/y from the **guest** pitch and
address but bounds them against `r->w` and `r->h`, the **host** allocation size.
`off_rt_get_impl` scales these dimensions before allocation (line 10086).
The existing `rt_gw`/`rt_gh` helpers explicitly recover guest dimensions
(line 10953), including the full-width race case.

Two arithmetic counterexamples, evaluated without launching a game:

- Guest 1280x720, host 1707x720, pitch 8192, four bytes per guest pixel:
  an address 5120 bytes after the start has x=1280/y=0. The current check
  accepts that address; it is outside the guest's active row and in padding.
- Guest 1920x1080, host 3840x2160, pitch 7680: the address immediately after
  the guest surface has y=1080. The current check accepts it as inside
  the target, up to host row 2159.

The sampling path calls this function after exact colour/depth/display
matches fail (line 13028). A false match replaces the intended uploaded
texture with an unrelated target. This is a plausible cause of missing or
changing UI content, especially after another screen has populated the target
pool. There is no Course Maker texture address/capture proving it happens here.
The padded pitch above is a synthetic example, not a measured Course Maker
surface. At 720p, horizontal scaling requires row padding for this particular
counterexample; tightly packed rows do not establish an overrun by themselves.

A candidate fix should use guest bounds (with declarations moved/forwarded
as needed), preserve genuine mip/subrectangle aliases and sample coordinates,
and check formats/pitches. Do not ship it as a Course Maker fix on this
evidence alone. Existing target growth/reuse and guest-format byte sizes also
need consideration when validating containing-address matches.

## Remaining alternatives

- Glyph/stencil-mask generation or sampling: consistent with missing labels
  while bitmap icons and the map survive. Inspect the actual stencil ops,
  mask target and consuming draws before changing mask classification.
- Incomplete frames presented between editor passes: consistent with flicker.
  The backend has multiple presentation paths; an FPS counter alone cannot
  separate full frames from partial batches.
- Missing/compiling pipelines: possible, especially in a cold editor. Check
  whether the problem persists after compilation drains and on re-entry.

## Original capture plan (retained for follow-up)

1. Record build identity and settings. Enter Course Maker on the same save,
   retain the cache, wait for compilation, and compare first entry/re-entry.
   Start a private run only when no other game/test owner is using the slot.
2. Capture several consecutive editor frames with `RSX_CAP`, a private
   `RSX_CAP_DIR`, and `RSX_CAP_ALLRT=1`; retain manifests and colour/mask RTs.
   Use `PAD_FILE_LOG=2` to measure a Course Maker marker before defining any
   target-specific screen milestone. No Course Maker marker is measured here.
3. Correlate missing panel/text draws with their sampled addresses and target
   extents. `RT_TEXDBG` provides selected alias decisions; inspect its bounded
   log coverage before treating absence of a line as evidence.
4. Compare 16:9 with 21:9 at the same guest resolution, then a private candidate
   using guest bounds. A broad diagnostic `RT_NO_TEXSCALE=1` disables the
   alias/scaling block but also breaks legitimate RTT sampling; it is only a
   discriminator, never a proposed player workaround.
5. If masks are wrong, trace their producer/consumer ordering and stencil ops.
   If full captures are right while presentation flickers, trace flip/clear
   boundaries instead. Keep these hypotheses separate until captured.

The original source lead is retained as a separate issue. The private bounds
diagnostic reported no rejected containing-address matches in the corrected
21:9 run. The production correction does not change texture lookup.
