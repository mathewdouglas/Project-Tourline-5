# Missing-audio investigation and invalid PCM handling

The live investigation found invalid floating-point samples in GT5's guest
audio ring while its playback index advanced. A successful WASAPI open and
an unmuted Windows session do not establish that the game is producing valid
sound. The origin is now found and fixed: see "Root cause" below. Menu and
race sound play again (2026-11-14).

The audio change has two independent parts:

- The jitter-buffer data predicate checks every sample in every channel.
  Previously it skipped stereo's right channel and seven surround channels.
- Nonfinite guest samples are counted per port. A corrupt port's entire
  block is excluded from the downmix, while its ring is still consumed.
  Replacing individual NaNs alone left a clipped 6 kHz pattern in live checks;
  the rest of a corrupt block cannot be presumed healthy. Healthy ports remain
  intact, so movie audio can survive a broken game-music port. A second check
  at the mixed-output boundary clears any remaining invalid result.
  This containment does not repair decoder/DSP state already producing bad data.

`AUDIO_NONFINITE_SCRUB=0` restores propagation for diagnosis. It disables both
input and mixed-output scrubbing. It is not an audio-repair setting.

## Root cause: two SPU arithmetic differences (2026-11-14)

The PCM ring is written by audio image 14 (`jFDFFAAC1`), which copies an
already corrupt buffer fetched from guest `0x4209EB30`. Stage measurements
traced that buffer to the DSP image `j9804ABA8`: its input decayed toward
silence while its output grew exponentially, first as a large DC offset,
then Inf/NaN after about 43 s on the menu. Two causes, both needed:

1. **Extended-range values times zero.** DSP initialisation takes square
   roots of zero. The SPU reciprocal-sqrt estimate of 0 has exponent 255,
   which the SPU treats as a finite extended value, not IEEE Inf. The SPU
   also flushes zero/denormal operands, so `0 * extended` is 0 there; the
   host computed NaN and stored it in the DSP tables. The force-included
   `src/gt5p_spu_audio_math.h` makes `fi`, `fm`, `fma`, `fms` and `fnms`
   return the SPU's result for exactly that operand pair and interpolates
   `fi` on extended estimates. Ordinary operands keep the runtime's
   arithmetic, FMA dispatch included. It is not a full extended-range
   implementation. Applied to `audio_extended_zero_images` in
   `config/targets.json` (the four native audio images plus `j9804ABA8`);
   render images are excluded. CMake `-DGT5P_AUDIO_EXTENDED_ZERO=OFF` is the
   build-time rollback.
2. **Rounding of the filter coefficients.** SPU single precision truncates;
   the host rounds to nearest. The coefficient constructor at `0x4E8` in
   `j9804ABA8` builds a 1 Hz filter whose denominator margin is
   `-5.96e-08` under round-to-nearest: one pole at about 1.000125, just
   outside the unit circle, hence the slow DC build-up. Truncation gives
   `+1.79e-07` (stable). `spu_audio_round_zero_calls` lists the five call
   sites; `scripts/spu_native_patch.py` (already a required post-SPU-lift
   step) wraps each in a round-toward-zero MXCSR scope and restores the
   caller's mode afterwards. Sample processing keeps its existing mode.
   `GT5P_AUDIO_ROUND_ZERO=0` turns the scope off at run time.

Fixing only one image, or only cause 1, left the failure in place: earlier
partial attempts still produced the DC offset and NaNs.

Tests (synthetic, no game run): `scripts/test/audio_extended.py` checks
zero/extended products, interpolation, ordinary-operand parity and sqrt(0)
in portable, scalar-reference and FMA builds. `scripts/test/audio_coefficients.py`
runs the real lifted constructor (extracted from `generated/` into
`scratch/` at test time) on synthetic inputs, checks the stability margin
and a million-sample impulse tail, the rollback, MXCSR restoration and the
patcher's exact-count refusal, with the extended fix on and off.

Live, build `6e224f6e102d252a` (SHA-256 prefix), native audio,
`PS3_VERBOSE=0`, headless:

- Menu, scrubbing off: 77 s recorded, 0 NaN/Inf, no DC (5 s means within
  0.0004), peaks 0.28-0.35 after the music starts; past the old 43 s failure.
- HSR race, scrubbing off: 145 s, 0 NaN/Inf. A small steady offset of about
  0.012 appears once driving starts and does not grow; peaks reach 1.0
  and about 1% of samples clip in the loudest 10 s of throttle. Not shown
  to be wrong; listening is still owed.
- Opening movie, default scrubbing: 57 s, 0 NaN/Inf, peaks 0.5-0.7.

The containment below stays enabled: it is now a safety net, not the fix.

## 2.11 race NaNs: extended-range arithmetic (2026-11-15)

GT5 2.11's races put invalid samples in port 0 from race start. They are not
host-made NaNs. They are `0xFFFFFFFF` words: one whole channel, 256 per block.
Traced backwards through every SPU transfer (a private DMA scan, below):

- image 18 `socedmix` (`jFDFFAAC1`) writes them to the port ring;
- it copies them from `0x40A3BB10`, written by image 10 `socecomp`;
- that copies them from the bus `0x40A41B10`, where image 4 `soceamp`
  finds them already;
- image 2 `socepdideq` (`j0B07CA3D`) puts them there. That is the EQ, which
  changed in 2.11. Its own 0xD80-byte state block gains one more
  `0xFFFFFFFF` per tick from boot.

On the SPU, exponent 255 is an ordinary finite value up to about 2^129. A
reciprocal estimate of 0 is one, and `0xFFFFFFFF` is the most negative
value. Multiplied by a small coefficient it gives a normal number. The host
reads the same bits as NaN and propagates the payload, so one extended value
in the EQ state poisoned its output once the race used that path.
v104's fix covered only `0 x extended`. Running the whole image in
round-toward-zero, or on the SPU interpreter, did not help.

The fix is in `src/gt5p_spu_audio_math.h`, for the same
`audio_extended_zero_images`:

- `fa`, `fs`, `fm`, `fma`, `fms`, `fnms`, `fceq` and `fcgt` recompute, in
  double precision with the SPU's reading of exponent 255, any lane where an
  operand or the host result has exponent 255;
- results truncate, saturate at `+-0x7FFFFFFF` and flush below 2^-126;
- every other lane keeps the runtime's result, so ordinary arithmetic is
  unchanged;
- an SSE test skips the slow path when no lane needs it;
- `GT5P_AUDIO_EXTENDED_RANGE=0` turns it off. It is not exact for fused ops
  at the rounding edge.

Live, 2.11 High Speed Ring at 720p, scrubbing off (`AUDIO_NONFINITE_SCRUB=0`):

- three default runs had 0 invalid-PCM reports, with peaks up to 0.36-0.38;
- the same build with `GT5P_AUDIO_EXTENDED_RANGE=0` brought the NaNs back
  (peak 1.0);
- extended values still pass between the EQ stages, as on the SPU, but the
  port output stays finite.

The races still freeze later; that is separate. v104 is not re-measured with
this change. It affects only lanes that the old code turned into NaN or Inf,
but a v104 race listen is owed. (It was owed: see the 0.1.8 section below.)

`scripts/test/audio_extended.py` adds these checks, in portable, scalar and
FMA builds: extended products, saturation, truncation, sums, compares, and
200,000 random extended lanes against an independent model.

The DMA scan that found this (`SPU_NANSCAN`, with `SPU_NANSCAN_RANGE` and
`SPU_NANSCAN_IMG`) is a private runtime diff, `scratch/v211/nanscan.diff`.
It is not in the runtime patch.

## Surround output (0.1.9, issue #21)

GT5 opens its audio ports with 8 channels and fills them in the PS3 order
L R C LFE RL RR SL SR. `AUDIO_CHSTAT=1` (runtime) logs each channel's RMS
every 1000 blocks. High Speed Ring race, Zonda R, in-car view (0.1.8 engine):

| | L | R | C | LFE | RL | RR | SL | SR |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| RMS | 0.04-0.28 | 0.04-0.11 | 0.05-0.10 | 0-0.15 | 0.19-0.30 | 0.18-0.31 | 0 | 0 |

So GT5 mixes discrete 5.1 (the rear pair carries the mid engine) and never
writes the side pair. Reporting 7.1 instead of stereo through
`cellAudioOutGetConfiguration`/`GetState` changed nothing: it mixes 5.1
regardless. The mixer used to fold everything to stereo.

`AUDIO_OUTPUT_CHANNELS` (runtime; `audio.output` = `"stereo"`, `"5.1"`,
`"7.1"`, `"auto"`; F1 AUDIO > Sound output) picks the device layout: 2
(default, the old downmix), 6 (FL FR FC LFE BL BR; the side pair is folded
into the rears at 0.707), 8 (all eight, channel mask 0x63F), or auto (the
device mix format's channel count). Channels pass through unchanged; the PS3
order is Windows' order. The same value is reported to the title. A layout
the device refuses falls back to stereo with a log line; on this machine's
stereo device Windows accepted 6 and 8 and folded them itself. Race
underruns: stereo 1, 5.1 3, 7.1 12, auto (stereo) 6, all within normal
startup. Speaker placement has not been checked on real surround hardware.

## 01.04 race stutter from the extended-range check (fixed 0.1.8)

The "v104 race listen" owed above was not done before 0.1.6 shipped, and
01.04 race audio stuttered in 0.1.6 and 0.1.7. Same High Speed Ring route at
720p/2x MSAA (`AUDIO_RATE=1 SPURS_JC_STATS=1`):

| Engine | Jobs/s | Per job | Chain busy | Underruns | Silent blocks |
| --- | ---: | ---: | ---: | ---: | ---: |
| 0.1.5 | 2,250 | 0.38 ms | 84% | 6 | 0% |
| 0.1.7 | 1,450 | 0.67 ms | 98% | ~360 | ~30% |
| 0.1.7, `GT5P_AUDIO_EXTENDED_RANGE=0` | | | | 274 | 19% |
| 0.1.8 build, no range check | 2,251 | 0.36 ms | 80% | 11 | 0% |

The lifted audio code was identical between the two releases. The check's
lane test (and, at run time, a `volatile` flag read) on every float op made
the job chain CPU-bound, so turning it off at run time recovered only part.
Undoing 0.1.6's other changes (`SPURS_LANE_RETRY=1`, `SPURS_LANE_JOIN=0`,
`PS3_TLS_FROM_CRT=0`) changed nothing. Menus were unaffected.

The check is now built only for targets with `"audio_extended_range": true`
in `config/targets.json` (2.11), as `GT5P_AUDIO_EXTENDED_RANGE_BUILD=1`.
Without it the header is 0.1.5's arithmetic (only `0 x extended = 0`).
`audio_extended.py` runs every mode with and without the range build. 2.11
keeps the cost; its race audio has not been measured for this.

## Diagnostics

`AUDIO_RATE=1` reports finite silence separately from invalid blocks, mixed
NaN/Inf sample counts and input NaN/Inf sample counts. `AUDIO_PEAK=1` reports
finite peak/nonzero counts plus input and mixed invalid counts. Scrub status
is explicit. Invalid input is also reported by port at most every five
seconds without requiring either diagnostic switch. A NaN no longer counts
as proof of non-silent sound.

Startup COM, endpoint-enumerator, default-output and endpoint-ID failures
report operation names and HRESULTs. Endpoint retries are rate limited to
one identical error per five seconds. No system device or format is changed.

`AUDIO_WAV=<path>` retains its existing raw f32le stereo, 48 kHz format. It
captures the actual mixed samples submitted by the mixer (after scrubbing
when enabled). Recordings and game traces belong under ignored scratch
storage and must not be committed.

## Regressions and live procedure

- `scripts/test/audio_silence.py`: actual predicate, every single-sample
  position/channel, finite silence and channel bounds.
- `scripts/test/audio_health.py`: production mixer with fake COM, NaN/Inf
  classification, byte-preservation of finite samples, valid movie audio
  mixed with a broken port, rollback, startup HRESULT/error-rate checks.
  No real device or game is opened.
- `scripts/test/audio_live.ps1 -Exe <private gt5.exe> -Route menu|movie|race
  -Tag <unique name> -Native 0|1 -Scrub 0|1`: waits for the machine to be free
  and delegates to the process-owned gt5boot runner. Uses PS3_VERBOSE=0,
  AUDIO_RATE/AUDIO_PEAK and a scratch-only raw recording. It never moves or
  deletes shader caches. Coordinate with other test owners before starting
  a batch; a quiet-process check cannot reserve the next owner's turn.
  `-DefaultScrub` tests the default with its environment variable absent,
  restoring this script process's previous environment afterwards.

Live results are recorded in [progress-gt5.md](progress-gt5.md). Invalid PCM
was reproduced in menu, opening movie and race, with native and portable
menu paths both affected. Containment alone restored the movie but left menu/race silent; the
root-cause fix above restores them. Subjective
speaker/headphone listening remains separate from signal/meter verification.
