"""Replay SPU_JOB_FP_TRACE per worker, without running a game or hashing assets."""
import argparse
import json
from pathlib import Path


def replay(rows, slots):
    workers = {}
    hits = misses = hashed_bytes = 0
    for worker, address, size, sample, fingerprint in rows:
        memo, next_slot = workers.setdefault(worker, ([None] * slots, 0))
        key = (address, int(size), sample)
        if key in memo:
            hits += 1
        else:
            misses += 1
            hashed_bytes += int(size)
            memo[next_slot % slots] = key
            workers[worker] = (memo, next_slot + 1)
    return dict(slots=slots, workers=len(workers), hits=hits, misses=misses,
                hashed_bytes=hashed_bytes)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('trace', type=Path)
    ap.add_argument('--global-cache',action='store_true',help='replay the original shared-cache topology')
    args = ap.parse_args()
    rows = [line.split() for line in args.trace.read_text().splitlines()]
    if any(len(row) != 5 for row in rows):
        raise SystemExit('Incomplete/interleaved trace row: capture again')
    if args.global_cache:
        rows=[['global',*row[1:]] for row in rows]
    print(json.dumps([replay(rows, n) for n in (8, 16, 32, 64, 128)], indent=2))


if __name__ == '__main__':
    main()
