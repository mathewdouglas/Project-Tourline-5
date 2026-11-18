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
| `gpu-stamp-before-start.patch` | GPU/driver stamp beside the pipeline blobs and the parallel shader preparation before start (GT5 Phase 44; `libs/video/rsx_d3d12_backend.c` only) | main patch at `f124011` |
| `spu-render-pass.patch` | Drain fast return (`SPU_DRAIN_RET_FAST`) and early user-command delivery from `usleep` (`PS3_USLEEP_GCM_WAKE`) (GT5 Phase 44; `runtime/spu/spu_context.h`, `spu_drain.c`, `runtime/syscalls/sys_timer.c`, `libs/video/cellGcmSys.c`) | main patch at `0860fd2` |
| `lane-pin-scope.patch` | SPURS lane pinning follows the race scope (`SPURS_LANE_PIN_SCOPE`) and the two-set pinning A/B (`SPURS_LANE_PIN_AB=3`) (GT5 Phase 46; `libs/spurs/cellSpurs.c`, `libs/video/rsx_d3d12_backend.c`) | main patch at this branch's Phase 46 commit |
| `issue43-sync-slot-free.patch` | lwcond slots freed when the last waiter leaves, signals claim none (`PS3_LWCOND_SLOT_FREE`); lwmutex slots freed on destroy (`PS3_LWM_SLOT_FREE`); issues #34/#37/#41/#43/#44/#45 (`runtime/ppu/ppu_sysprx.cpp`; third section of the main patch) | main patch after `issue37-lwmutex-host-state-log-caps-report-ring.patch` |
| `v211-freeze-flag-kick.patch` | Workload-flag kick refused by an ending pass is put back (`SPURS_FLAG_KICK_KEEP`); `SPURS_KEVLOG` logs lane passes, exits and joins, 256 events in every stall dump, with each queue's jobs around its head; 2.11 freeze #28/#36/#38/#39/#47 (`libs/spurs/cellSpurs.c`; fourth section of the main patch) | main patch after `issue43-sync-slot-free.patch` |
| `dlss-scene-boundary.patch` | DLSS step 1: scene/HUD boundary judged per frame, logging only (`RSX_SCENE_LOG`; `libs/video/rsx_d3d12_backend.c` only) | main patch at `22c06e0` |
| `dlss-scene-consts.patch` | DLSS step 2 groundwork: `RSX_SCENE_CONSTS` writes the scene draws' vertex constants; the scene check reads both switches (`libs/video/rsx_d3d12_backend.c` only) | main patch after `dlss-scene-boundary.patch` |
| `dlss-camera-motion-vectors.patch` | DLSS step 2: camera motion vectors from depth per viewport (`RSX_MV`, `RSX_MV_VIEW`, `RSX_MV_LOG`, `RSX_MV_MIRROR`, `RSX_MV_CHECK_SCALE`; `libs/video/rsx_d3d12_backend.c` only) | main patch after `dlss-scene-consts.patch` |
| `dlss-object-motion-vectors.patch` | DLSS step 3: per-object motion vectors (`RSX_MV=2`: pipeline variant with a vector target, per-draw T, matching by world matrix and object-to-clip; `RSX_MV_MATCH_DC`, check readback `[mv] check`, `RSX_MV_VIEW=d`, `RSX_MV_OBJ_READ`, `RSX_MV_OBJ_MATCH`, `RSX_MV_LOG=2..4`; `libs/video/rsx_d3d12_backend.c` only) | main patch after `dlss-camera-motion-vectors.patch` |
