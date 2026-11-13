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
