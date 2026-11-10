"""Summarize the opt-in backend batch trace; no game launch or cache writes.

Stage counters overlap. Async completion counters are correlations, not the
blocking cost of this frame. Successful DXBC file reads include allocation and
memory-cache insertion, exclude misses, packs, native blob reads and guest I/O.
"""
import argparse
import csv
import io
import json
import math
from pathlib import Path

STAGES = ('tex_ms', 'vtx_ms', 'vs_ms', 'cb_ms', 'wait_ms', 'pre_ms',
          'srv_ms', 'pso_ms', 'record_ms')
COUNTERS = ('cache_us', 'cache_reads', 'shader_compiles', 'driver_calls',
            'driver_us', 'cached_calls', 'demand_queued', 'demand_done')
TEXTURE_COUNTERS = ('tex_alloc_ms', 'tex_decode_ms', 'tex_allocs', 'up_allocs',
                    'uploads', 'upload_bytes', 'tex_hash_ms', 'tex_map_ms',
                    'tex_copy_ms', 'tex_srv_ms')


class TraceRows(list):
    discarded_tail_bytes = 0


def read_trace(path, allow_incomplete_tail=False):
    if path.stat().st_size > 32 * 1024 * 1024:
        raise ValueError('Trace exceeds 32 MiB bound')
    rows = TraceRows()
    data = path.read_bytes()
    if data and not data.endswith(b'\n') and allow_incomplete_tail:
        end = data.rfind(b'\n') + 1
        rows.discarded_tail_bytes = len(data) - end
        data = data[:end]
    with io.StringIO(data.decode('ascii'), newline='') as stream:
        for raw in csv.DictReader(stream):
            if len(rows) >= 32768 or None in raw or any(v is None for v in raw.values()):
                raise ValueError('Oversized or incomplete trace')
            row = {k: float(v) for k, v in raw.items()}
            if any(not math.isfinite(v) or v < 0 for v in row.values()):
                raise ValueError('Non-finite or negative trace value')
            for key in ('unix_ms', 'qpc_ms', 'batch_ms', 'present', 'foreground',
                        'present_ms', 'gpu_previous_ms', 'pending', *STAGES, *COUNTERS):
                if key not in row:
                    raise ValueError('Missing trace column: ' + key)
            if row['present'] not in (0, 1) or row['foreground'] not in (0, 1):
                raise ValueError('Invalid trace flag')
            cumulative = (*STAGES, *COUNTERS, *(k for k in TEXTURE_COUNTERS if k in row))
            if rows and (row['qpc_ms'] <= rows[-1]['qpc_ms'] or
                         any(row[k] < rows[-1][k] for k in cumulative)):
                raise ValueError('Non-monotonic clock or reset cumulative counters')
            rows.append(row)
    if len(rows) < 2:
        raise ValueError('Insufficient trace')
    return rows


def summarize(rows, grid_ms=0, shots=(), threshold=50):
    events, previous, batches = [], None, []
    for row in rows:
        batches.append(row)
        if not row['present']:
            continue
        if previous is not None:
            event = dict(unix_ms=row['unix_ms'], grid_seconds=(row['unix_ms']-grid_ms)/1000,
                         interval_ms=row['qpc_ms']-previous['qpc_ms'],
                         max_batch_ms=max(r['batch_ms'] for r in batches),
                         present_ms=sum(r['present_ms'] for r in batches),
                         foreground=all(r['foreground'] for r in batches),
                         gpu_previous_ms=row['gpu_previous_ms'], pending=row['pending'])
            cumulative = (*STAGES, *COUNTERS, *(k for k in TEXTURE_COUNTERS if k in row))
            event['deltas'] = {k: row[k]-previous[k] for k in cumulative}
            event['shot_overlaps'] = [s['file'] for s in shots
                                      if s['start_ms'] <= row['unix_ms'] and
                                      s['end_ms'] >= previous['unix_ms']]
            events.append(event)
        previous, batches = row, []
    if not events:
        raise ValueError('No presented intervals')
    intervals = sorted(e['interval_ms'] for e in events)
    return dict(presented_intervals=len(events),
                discarded_tail_bytes=getattr(rows, 'discarded_tail_bytes', 0),
                foreground_intervals=sum(e['foreground'] for e in events),
                trace_at_cap=len(rows) == 32768,
                interval_p99_ms=intervals[math.ceil(.99*len(intervals))-1],
                over_threshold=sum(e['interval_ms'] > threshold for e in events),
                hitches=sorted((e for e in events if e['interval_ms'] > threshold),
                               key=lambda e:e['interval_ms'], reverse=True)[:100],
                limits='Rendered display batches, not independently confirmed displayed frames. '
                'Stage times overlap; async completions are not critical-path durations. '
                'GPU timing is the previous measured batch. Unmeasured gaps include guest work, '
                'streaming, scheduling and synchronization; they are not automatically guest CPU.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('trace', type=Path)
    parser.add_argument('--window', type=Path)
    parser.add_argument('--shots', type=Path)
    parser.add_argument('--threshold', type=float, default=50)
    parser.add_argument('--allow-incomplete-tail', action='store_true',
                        help='Explicitly discard and report a killed writer\'s final unterminated row')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if not math.isfinite(args.threshold) or args.threshold <= 0:
        parser.error('Threshold must be finite and positive')
    window = json.loads(args.window.read_text(encoding='utf-8-sig')) if args.window else {}
    shots = json.loads(args.shots.read_text(encoding='utf-8-sig')) if args.shots else []
    result = summarize(read_trace(args.trace, args.allow_incomplete_tail),
                       window.get('grid_observed_ms', 0), shots, args.threshold)
    with args.output.open('x') as stream:
        json.dump(result, stream, indent=2)


if __name__ == '__main__':
    main()
