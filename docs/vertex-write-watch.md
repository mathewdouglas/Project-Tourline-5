# Vertex-cache write tracking prototype

Implemented 2026-11-12 for the current BCUS98114-v104 runtime. **Off by
default. No live performance gain has been measured.** Claude's separate
SPU investigation was running game measurements during this work; no game
was launched or stopped, and its checkout/runtime/build were not changed.

## Purpose and scope

Our cross-frame raw vertex cache compares guest bytes with its mirror.
After eight clean frames its existing fast path compares a whole stream
once per frame. The prototype instead asks Windows which guest pages have
been written and skips byte comparisons only when the cached stream's
resolved pages remain clean. It checks each use, including later draws in
the same frame. A write to an unrelated byte in a shared page conservatively
forces a comparison.

The idea comes from [MormonFTP-hub/gt5-recomp at 52369f8](https://github.com/MormonFTP-hub/gt5-recomp/blob/52369f843d11f7e734171895f6783b5ac0b70a33/GT5/patches/ps3recomp.patch),
which uses shared page generations for converted geometry and textures.
This implementation is written for our raw streams, existing cache and
non-contiguous GCM IO mappings. It does not replace the renderer, change
SPU execution, or track textures yet.

## Switches

- `RSX_VTXCACHE_WRITE_WATCH=0` (default/unset): existing allocation and
  cache-validation behavior.
- `RSX_VTXCACHE_WRITE_WATCH=1`: reserve the guest arena with
  `MEM_WRITE_WATCH`; skip clean cached-stream comparisons.
- `RSX_VTXCACHE_WRITE_WATCH=2`: same tracking, but byte-check every clean
  hit against the full cached stream. A mismatch logs `[write-watch]
  MISMATCH`, disables skipping for the session and refreshes the stream.
- `RSX_VTXCACHE_VERIFY=draw`: retains full byte verification even with
  write-watch mode 1.
- `PS3_NO_WRITE_WATCH=1`: reserve without tracking, for fallback testing.
  Presence of this variable disables tracking allocation (unset it to enable).
- `RSX_VTXCACHE=0`: existing complete vertex-cache rollback.

Restart after changing the write-watch mode; the reservation flag is chosen
at VM initialization. Rebuild before testing: this change has not been
linked into the existing game executable.

`RSX_VTXSTAT=1` prints the backend's existing vertex statistics and the new
cumulative `[write-watch]` counters: hits, clean bytes eligible for skipping,
queries, dirty pages, failures and verification mismatches. Mode 2 still
compares its clean bytes, so that counter is not a measure of work avoided
in verification runs.

## Correctness details

`libs/video/rsx_write_watch.h` owns one shared generation table for the FIFO
thread. Every reset harvests dirty pages into that table, so a second cache
entry cannot erase the first entry's invalidation. Future cache consumers
must use the same state rather than independently resetting Windows' bits.

Snapshots are taken before copying or comparing. A write during that read
therefore remains visible at the next query. Each snapshot also stores its
exact resolved EA ranges. The backend resolves every 1 MB IO page on every
use and compares those ranges, so clean pages at a newly mapped address
cannot validate a copy from an old mapping. Adjacent physical ranges merge
into one query; non-contiguous ranges remain separate.

On a dirty snapshot, the entire stored stream is compared before a new
snapshot can validate it. Comparing just the current draw's subrange would
incorrectly validate the rest of the stream. Failed/unavailable queries
fall back to whole-stream comparisons on every use, without the legacy
once-per-frame shortcut. The 4 GB alignment required by HLE pointer
conversion is retained. If Windows refuses the tracked reservation, VM
initialization retries the ordinary reservation.

The tracker uses approximately 8 MiB for page generations when enabled;
the snapshot fields add approximately 0.8 MiB to the allocated cache table
in either mode. It does not install page guards or instrument guest stores.
Like the existing cache, it relies on the guest's producer/consumer ordering;
it does not make concurrently modified geometry an atomic snapshot.

The query/reset operation uses `WRITE_WATCH_FLAG_RESET` in one call, following
[Microsoft's API contract](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-getwritewatch).
Separate query and reset calls can lose writes between the two operations.

## Validation completed

`python scripts/test/vertex_write_watch.py` builds a small Windows test with
the actual tracker header, the actual `vc_place`/copy/compare functions
extracted from the runtime, and the actual VM reservation loop. It uses no
game data or GPU. Passed on the freshly prepared runtime in modes 0, 1, 2,
and mode 1 with `RSX_VTXCACHE_VERIFY=draw`:

- two entries sharing a page, one harvesting the other's write;
- writes after the snapshot begins, unaligned and disjoint ranges;
- address overflow and the final page of the 4 GB arena;
- snapshot lifetime changes and unavailable tracking;
- cache mutation after the eight-frame stability threshold;
- non-contiguous IO mappings and remapping to previously clean data;
- comparison fallback and deliberate stale-cache verification failure;
- real tracked/untracked allocation, invalid mode and forced fallback,
  with 4 GB alignment preserved.

The full D3D12 backend also passed clang-cl's C17 syntax check. The updated
compatibility patch applied cleanly to a fresh pinned runtime and passed
`scripts/setup_runtime.py --check`. Unrelated sections of the original
compatibility patch were retained byte-for-byte. The single-change copy is
`patches/incremental/vertex-write-watch.patch`; setup does not read it.

## Live measurement still required

Only begin when the other thread's game measurements have finished. Do not
launch a second `gt5.exe` or stop a process owned by another test.

1. Build with this runtime and run mode 2 through the car list, HSR racing,
   Eiger, and repeated menu/race transitions. Require zero real verification
   mismatches and inspect the game window for rendering regressions.
2. With a warm DXC shader cache, compare repeated HSR frame tests in an
   alternating 0/1/1/0 order, holding all other switches constant. Use
   `PS3_VERBOSE=0`, `RSX_PRESENT_SYNC=0`, `RSX_VTXSTAT=1`, and the same
   resolution, sample delay and race route.
3. Record FIFO walk CPU time, per-thread CPU, mean FPS, p99/late frames,
   comparison bytes, clean hits, query count and tracker failures. Include
   the game build/commit and the raw logs. Mode 2 is for correctness, not
   timing.
4. Leave the feature off unless the complete frame measurements show a
   repeatable benefit. Queries on small streams and write-watch overhead on
   guest stores could cost more than the comparisons saved. No percentage
   speedup is predicted from the other repository's results.

The existing `scripts/test/gt5boot.ps1 -Route race -FrameTest -Cpu` supplies
the route and timing window. Use distinct tags, and do not combine this A/B
with Claude's SPU/scheduler changes. Texture integration remains a separate
experiment after vertex tracking has live evidence.
