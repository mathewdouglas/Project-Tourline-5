# Fatal renderer errors

Implemented following issue #7 (race-loading picture frozen while the guest/audio
continued). The failure paths were exercised with injected failures on
2026-11-16 (below). This is error handling, not a fix for the invalid rendering
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

## Failure injection

`RSX_FAIL_TEST=<op>[:<frame>]` (runtime backend) injects one failure at or
after that frame (default 600); unset, it never fires:

- `close`: the frame command list's Close returns `DXGI_ERROR_INVALID_CALL`;
- `present`: Present returns `DXGI_ERROR_DEVICE_HUNG` without presenting;
- `remove`: `ID3D12Device5::RemoveDevice` before the frame's submit, a real
  device removal that every later device call then sees.

`scripts/test/renderer_fatal.ps1 -Work <folder> -Mode silent|noreport|dialog|direct -Op <op>`
drives it through the launcher or the engine alone, presses the dialog
buttons through UI Automation and saves a shot of each dialog.

## Close failed on Windows 10 only (issues #16, #18, #19; fixed 0.1.8)

`frame command list Close hr=0x80070057 removed=0x00000000` while a race
loads or in the dealership, reported only on Windows 10 build 19045 (RTX
3070, 3090 Ti, 4060 Ti; DXC and FXC). Close returns `E_INVALIDARG` for an
invalid argument recorded earlier in the frame, and Windows 11's runtime
(where the debug layer was clean) accepts things Windows 10's does not.

`RSX_CMD_CHECK=1` wraps the frame command list (and the device's
CreateRootSignature) and logs, as `[cmd-check]`, each kind of command an
older runtime rejects: placed-footprint offsets not 512-aligned, pitches not
256-aligned, BC footprints not block-sized, copy boxes past either resource,
subresource indexes out of range, copies between dimensions or formats,
CopyResource/ResolveSubresource shape mismatches, invalid viewports and
scissors, barriers with equal states or to states the resource cannot take,
root bindings of the wrong type or index for the bound signature, null
PSOs/handles. Every 600 frames it prints how many calls it checked. The
runtime swaps the list's method table on Close/Reset, so the wrapper is put
back after every Reset. Diagnostic only: calls are forwarded unchanged.

At 720p/2x MSAA, High Speed Ring, it reported exactly one thing, in the first
race frame (just before `[ZONLY] depth-only surface 0xA87BE00`, where the #16
log has its fatal line): a 1x1 RGBA8 cube texture uploaded with faces 1, 3
and 5 at 256-byte offsets (`pitch * rows` = 256 per face). Faces now start
on 512-byte boundaries of the staging buffer (`RSX_TEX_CUBE_ALIGN=0`: packed
as before, which brings the three reports back). After the fix: no report in
about 760,000 checked copies over a race and the car list; 58.5 FPS; the car
list renders the same. Not run on Windows 10 itself.

Seen while checking, not changed: the MSAA depth-resolve pass draws with
whatever root signature the last draw bound (debug-layer ID 201; its PSO uses
`vp_root_sig`), and DXC fallback pipelines that pair the vs_5_0 fallback
vertex shader with an SM6 pixel shader fail creation (ID 1046).

## Validation

**2026-11-16, 2.11 engine (`gt5_211.exe`) and launcher built from this
branch, private launcher home, one game at a time.** Every run printed
exactly one `[renderer-fatal] operation=` line, then `[renderer-report]`
and the diagnostic snapshot, and the engine exited with `0x50543547`.

| Run | Op | Result |
|---|---|---|
| launcher, silent (`PT5_SETUP_UI=0`) | close:300 | `hr=0x887A0001`; report ZIP saved, launcher exits, no restart |
| launcher, silent | present:300 | `hr=0x887A0006`; report saved |
| launcher, silent | remove:300 | `operation=frame ExecuteCommandLists hr=0x887A0005 removed=0x887A0005`; report saved |
| launcher, silent, `RSX_DRED=1`, empty shader cache | remove:1500 | DRED lines (`every command list completed`, `no page fault`) in the log |
| launcher, silent, `PT5_CRASH_REPORT=0` | close:300 | "Graphics rendering stopped. See logs\play.err."; no ZIP |
| engine alone | present:300 | its message box (code `0x887A0006`); OK exits `0x50543547` |
| engine alone, driven to the menu, empty cache, `RSX_DRED=1` | remove:4500 | at the menu (486 draws a frame, 289 pipelines built, helpers active); one fatal line, message box, exit |
| launcher dialog | remove:300 | dialog with Report / Save only / Save report and restart / Close; "Save report and restart" saved a ZIP holding the first run's `play.err` (scrubbed; its host thread ids match the first run, not the restart), then started a fresh engine; that engine's failure showed the dialog again (no automatic restart loop); Close ended the launcher with no engine left |

The ZIP holds `README.txt`, `summary.md`, `system.txt`, `play.err`,
`play.out` and the settings file; the summary names the operation, HRESULT
and build. Not exercised: a real driver fault (issue #7), the "Report this on
GitHub" and "Save the report only" buttons (they share the save path with
restart), minimized/occluded windows against the present watchdog, and a v104
engine (same source; built for release).

## Validation history

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
