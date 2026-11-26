"""Guest copies keep the replaying frame's texture bytes instead of waiting
(RSX_COPY_SNAP_REPLAY).

A copy into memory a texture of the frame being replayed binds used to make
the FIFO walker wait (up to 30 ms) for that replay; at Monza with the field in
view that cost 3-4 ms a frame. The walker now appends a snapshot to the
replaying frame's table, as it already did for the recording frame. Checks
the safety rules in the source: entries are only appended and published after
they are filled, cubes and a full table fall back to the wait, and a frame
set's table is reset when it is taken for recording again.
"""
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RUNTIME = Path(os.environ.get("PS3RECOMP_DIR", ROOT.parent / "ps3recomp-tourline5"))


def main():
    src = (RUNTIME / "libs/video/rsx_d3d12_backend.c").read_text(encoding="utf-8")
    fn = src[src.index("void rsx_d3d12_before_guest_copy(u32 ea, u32 len)"):]
    fn = fn[:fn.index("/* The frame being recorded: keep the bytes its draws bound. */")]
    assert 'getenv("RSX_COPY_SNAP_REPLAY")' in fn, "missing rollback switch"
    assert "if (job->ts[k].cube) { ok = 0; break; }" in fn, "cubes must fall back to the wait"
    assert "if (n0 >= TEX_SNAP_MAX) { ok = 0; break; }" in fn, "a full table must fall back"
    fill = fn.index("memcpy(job->snap[n0].buf, vm_base + off, nb);")
    barrier = fn.index("MemoryBarrier();", fill)
    publish = fn.index("job->snap_n = n0 + 1;", barrier)
    assert fill < barrier < publish, "entry must be filled before it is published"
    assert "job->snap[j].nbytes < nb) ok = 0" in fn, "a shorter existing copy must not be replaced"
    assert "if (ok) hit = 0;" in fn and "while (s_rt_busy && GetTickCount64() - w0" in fn
    swap = src[src.index("static void rt_submit(void)"):]
    swap = swap[:swap.index("nxt->parity = job->parity ^ 1;")]
    assert "rt_wait_idle();" in swap, "the walker must own the frame set before resetting it"
    assert "nxt->snap[k].nbytes = 0;" in swap and "nxt->snap_n = 0;" in swap
    print("[PASS] replay snapshots: append-only, filled before published, fallbacks, reset at swap")


if __name__ == "__main__":
    main()
