# Incremental runtime patches

Each file here is one change to the ps3recomp runtime, as a diff against the
runtime with the main compatibility patch of the commit before it. They are
**already included** in `../ps3recomp-5244db9-local.patch`;
`scripts/setup_runtime.py` does not read this folder.

They exist for integration: when a runtime checkout already carries other
uncommitted edits, apply the single change there with `git apply <file>`
(or `patch -p1` when nearby lines moved) and refresh the main patch from the
result, instead
of replacing a whole section of the main patch.

| File | Change | Base |
| --- | --- | --- |
| `race-prewarm-pack.patch` | Race pack and helper-thread race preparation (`libs/video/rsx_d3d12_backend.c` only) | main patch at `8b6f586` |
| `lockline-sleep.patch` | Lock-line waiters sleep after 1 ms, holder recorded (GT5 Phase 42; `runtime/spu/spu_coherency.*`) | main patch at `625dd76` |
| `spu-render-pass.patch` | Drain fast return (`SPU_DRAIN_RET_FAST`) and early user-command delivery from `usleep` (`PS3_USLEEP_GCM_WAKE`) (GT5 Phase 44; `runtime/spu/spu_context.h`, `spu_drain.c`, `runtime/syscalls/sys_timer.c`, `libs/video/cellGcmSys.c`) | main patch at `0860fd2` |
