# GT5 2.11: logo-to-menu stall (2026-11-15)

The first 2.11 bring-up played menu music while the picture remained on the
GT logo. The frame thread waited for the GPUSyncArray job set, with the RSX
FIFO drained. This stall is fixed by publishing MAIN-memory RSX reports.

## Measurement

The frame thread calls `0x00B95248`, which polls `0x00B99444` for the other
buffer's job-set completion. In `v211w`, the GPUSyncArray header at
`0x01913300` had reached command 6 in its second list; that command was
`00000005 4010120C`. The referenced word stayed `0x00010000`.

The addresses vary with allocation. In the next probe, `v211x`, the pending
command referenced `0x4010110C`. The FIFO had issued GET_REPORT (`0x1800`)
with data `0x01101100`, using report DMA context `0xFEED0001` (MAIN).
The mapping was EA `0x40100000`, IO `0x00100000`, size `0x100000`.
Consequently the report destination was `0x40101100`, and the word being
waited on was its padding/completion field at +12.

The old handler updated only `s_report_data[index]`, a host HLE table. It
never wrote the mapped guest report, and this report's index was beyond that
table's range anyway. Receiver polling could not repair the missing write.
The earlier label-window/semaphore hypothesis was not established: the
label trace showed ordinary label offsets, not these report destinations.

## Change

`cellGcmProcessReportMethod` handles SET_CONTEXT_DMA_REPORT and GET_REPORT
through `rsx_process_method`, covering both FIFO paths. It retains the host
table and the existing synthetic report values. For an aligned, fully mapped
MAIN destination, it writes the big-endian timestamp and value, then clears
the completion word through the coherent store path. It refuses unaligned,
unmapped and unsupported-context destinations. `GCM_REPORT_MEMORY=0` restores
host-table-only behavior. LOCAL reports retain the old table behavior.

This supplies report completion; it does not implement accurate host GPU
occlusion counts. The timestamp/value/padding layout and completion behavior
are corroborated by RPCS3's
[GET_REPORT implementation](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/RSX/NV47/HW/nv4097.cpp).

## Validation and limits

- Private non-PGO 2.11 build, `v211y`: reached the main menu with its animated
  background; no present-stall report in 55 seconds. The user confirmed the
  menu was visible. Receiver polling was explicitly off.
- Same executable, `v211z-rollback`, with `GCM_REPORT_MEMORY=0`: reproduced
  the logo stall, reporting no present for 3 and 11 seconds.
- `scripts/test/gcm_report_memory.py` compiles the production report handler
  and checks mapped destinations beyond the host table, big-endian payload,
  completion published last, adjacent-memory preservation, invalid destinations,
  legacy table behavior and rollback in separate processes.
- Runtime library build and private relink pass. The cumulative patch applies
  to a fresh pinned 5244db9 checkout; `setup_runtime.py --check` passes there.

Evidence is private under `scratch/pv/v211w.*`, `v211x.*`, `v211y.*` and
`v211z-rollback.*`. No game data or lifted code is included in this document.
This is menu bring-up only: races, later screens, 01.04 regression play and
accurate report counts remain unchecked. Existing unhandled imports and
SPU interpreter fallbacks are separate bring-up work.
