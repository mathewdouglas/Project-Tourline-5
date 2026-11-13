# Missing-audio investigation and invalid PCM handling

The live investigation found invalid floating-point samples in GT5's guest
audio ring while its playback index advanced. A successful WASAPI open and
an unmuted Windows session do not establish that the game is producing valid
sound. The origin of those invalid samples has not yet been established.

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
menu paths both affected. Containment restores a healthy movie signal to
Windows but leaves menu/race output silent because their producer is still
corrupt. This is not a claim that the upstream audio fault is fixed. Subjective
speaker/headphone listening remains separate from signal/meter verification.
