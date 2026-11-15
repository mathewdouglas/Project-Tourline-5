# Fatal renderer errors

Implemented following issue #7 (race-loading picture frozen while the guest/audio
continued). **Built and smoke-tested only (2026-11-15); the failure paths are
not validated yet.** This is error handling, not a fix for the invalid rendering
operation in that report.

## Player behavior

A failed frame/upload command-list Close, failed Present, or detected device
removal latches a fatal renderer state. Submission and new pipeline jobs stop;
queued pipeline/shader work is cancelled. Workers already inside a compiler or
driver call may finish. The fatal path does not join them or attempt to rebuild
the device in the current process.

The engine records the first operation/HRESULT and removal reason, recent frame
summaries, available validation messages and optional DRED data, then publishes
the failure. The presentation owner adds the build/target and a diagnostic state
snapshot and exits with `0x50543547` (`pt5_restart::renderer_error`). Direct
launches show an error dialog; managed launches let the launcher show it after
the child has exited.

The launcher recognizes this exit status and `[renderer-fatal]` messages, and also
recognizes older `[D3D12] DEVICE REMOVED` logs. The player can save a graphics
report, open a pre-filled GitHub issue, or **Save report and restart**. Restart
saves the current logs/settings/system details to `logs/reports/graphics-*.zip`
before the next process can replace `play.out` and `play.err`. Nothing is posted
automatically. `PT5_CRASH_REPORT=0` keeps reporting disabled but still offers a
restart after the fatal error. Silent setup mode saves a report without opening
a dialog and never automatically restarts.

The visible-picture stall watchdog now uses successful, non-occluded DXGI
Present returns instead of calls into the backend. Minimized/occluded windows
reset its timeout. Successful API submission is not proof of physical display
scanout. Existing loop timing/profiling counters remain attempt counters.

The launcher also selects the longest/latest freeze snapshot for an issue
excerpt, rather than pairing the longest duration with the first snapshot.

## Diagnostics

Existing opt-in diagnostics remain available:

- `D3D12_DBG=1`: request the Windows graphics debug layer.
- `D3D12_IQ=1`: drain validation messages during normal rendering. Fatal handling
  also records the last available messages without this switch.
- `RSX_DRED=1`: collect breadcrumbs/page faults from device creation onward.
  If disabled, the report explicitly says breadcrumbs were not collected.

Reports retain the existing privacy scrubber and exclude game files, saves,
screenshots and shader code. A graphics error with reporting disabled is not
silently converted into a successful normal exit.

## Validation still required

Done on 2026-11-15: the 2.11 engine (`gt5_211.exe`) and launcher
(`tourline5.exe`) build against the refreshed compatibility patch (the patch
applies to a clean 5244db9 checkout). `gt5p_present.cpp` needed the generated
`gt5p_build_info.h`; CMake now gives it the build directory as an include path,
as for `gt5p_debug_export.cpp`. A 230 s High Speed Ring race on 2.11 ran at
60 FPS with no renderer-failure message: the normal path is unaffected. No
failure was injected, v104 was not rebuilt with it and no launcher dialog was
seen. Still required before release:

1. Build v104 and v211 against the refreshed compatibility patch, and compile the
   non-Windows stubs. Run the existing player-setup/report and renderer checks.
2. Exercise failed Close, invalid-call/removal reasons and failed Present with
   synthetic failure injection. Verify only the first failure is published,
   queued jobs are cancelled once, active job source ownership is retained, and
   replay waiters wake without new GPU submissions.
3. Confirm managed error dialog/report-only/report-and-restart, reporting off,
   silent mode, direct engine launch and cancellation. Ensure restarting keeps
   the failing run's ZIP and does not create an automatic restart loop.
4. Check ordinary rendering plus offscreen-only batches, shader holds,
   minimized/occluded windows, successful-present counting and genuine stalls.
5. Reproduce issue #7 with matching track/car field and validation/DRED enabled
   to find the offending operation. Do not assume this handling fixes its cause.

Static source review and patch applicability checks are the only validation
performed before handoff. No renderer settings or caches are changed.
