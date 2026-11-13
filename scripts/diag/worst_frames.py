"""Break down the worst frames of a run: what each one's time went to.

    python scripts/diag/worst_frames.py <timeline.csv> <run.err> [--hitch <hitch.csv>] [--top N]
                                        [--etw <etw_threads.csv>]

Inputs are one run's PS3_TIMELINE dump (with its "#qpc_zero_ms" line), its log
(for thread names), optionally the RSX_HITCH_LOG trace of the same run and a
per-thread state table derived from an ETW (WPR CPU profile) trace
(scripts/diag/etw_threads.py). Frames are the title's render-thread frames
(timeline event C: CPU and wall time between two SetPrepareFlip calls).

For each of the N longest frames it prints, on one time axis:
  - the render thread: CPU, blocking syscalls by kind, frame-pacer sleep, spin
    waits, and the rest (not running, or waits under 100 us);
  - every other guest thread's blocking waits and CPU-visible activity;
  - SPU workloads: dispatches and lane run time by workload;
  - the FIFO walker (R) and presents (P);
  - renderer builder jobs finishing in the frame (J: compiles, pipelines);
  - the renderer's replay batches and GPU time from the hitch trace;
  - with --etw: each named thread's running / ready (runnable but not
    scheduled) / waiting time, and which other threads ran on the CPU while
    the render thread was ready.
"""
import argparse
import bisect
import collections
import csv
import re

SC = {82: 'evflag_wait', 92: 'sema_wait', 102: 'mutex_lock', 107: 'cond_wait',
      130: 'eq_receive', 141: 'usleep', 142: 'sleep', 95: 'lwmutex', 97: 'lwcond_wait',
      113: 'lwcond_wait', 120: 'event_port', 124: 'rwlock'}
JOB = {1: 'shader compile', 2: 'pipeline (worker)', 3: 'pipeline (helper)'}


def load_names(timeline, log):
    names, gname = {}, {}
    for path in (log, log.replace('.err', '.out')):
        try:
            for line in open(path, errors='replace'):
                m = re.search(r'\[THREAD (\d+)\].*hosttid=(\d+)', line)
                if m:
                    names[int(m.group(2))] = 'g%s' % m.group(1)
                m = re.search(r'\[hostthread\] (\S+) hosttid=(\d+)', line)
                if m:
                    names[int(m.group(2))] = m.group(1)
                m = re.search(r'sys_ppu_thread_create tid=(\d+) name="([^"]*)"', line)
                if m:
                    gname['g' + m.group(1)] = m.group(2)
        except OSError:
            pass
    zero = None
    for line in open(timeline, errors='replace'):
        m = re.match(r'#thread (\d+) (.*)', line)
        if m and int(m.group(1)) not in names and m.group(2).strip() != '?':
            names[int(m.group(1))] = m.group(2).strip()
        m = re.match(r'#qpc_zero_ms ([\d.]+)', line)
        if m:
            zero = float(m.group(1))
    return names, gname, zero


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('timeline')
    ap.add_argument('log')
    ap.add_argument('--hitch')
    ap.add_argument('--etw')
    ap.add_argument('--top', type=int, default=8)
    ap.add_argument('--after-ms', type=float, default=0, help='only frames after this timeline time')
    args = ap.parse_args()

    names, gname, zero = load_names(args.timeline, args.log)

    def who(tid):
        n = names.get(tid)
        if n is None:
            return 'tid%d' % tid
        return gname.get(n, n)

    ev = []
    for line in open(args.timeline, errors='replace'):
        if line.startswith('#'):
            continue
        us, tid, k, a, b = line.strip().split(',')
        ev.append((int(us), int(tid), k, int(a), int(b)))
    ev.sort()
    times = [e[0] for e in ev]
    frames = [e for e in ev if e[2] == 'C' and e[0] >= args.after_ms * 1000]
    if not frames:
        raise SystemExit('no render-thread frames (C events) in the window')
    rt = frames[0][1]
    walls = sorted(e[4] for e in frames)
    print('render thread %s: %d frames, median %.1f ms, p95 %.1f ms, worst %.1f ms'
          % (who(rt), len(frames), walls[len(walls) // 2] / 1000, walls[int(len(walls) * .95)] / 1000, walls[-1] / 1000))

    hitch = []
    if args.hitch:
        for r in csv.DictReader(open(args.hitch)):
            hitch.append((float(r['qpc_ms']), r))
        hitch.sort(key=lambda x: x[0])
    hq = [h[0] for h in hitch]

    etw = collections.defaultdict(list)   # tid -> [(start_us, end_us, state, other)]
    if args.etw:
        for r in csv.DictReader(open(args.etw)):
            etw[int(r['tid'])].append((float(r['start_us']), float(r['end_us']), r['state'], r.get('detail', '')))

    for f in sorted(frames, key=lambda e: -e[4])[:args.top]:
        t1, wall, cpu = f[0], f[4], f[3]
        t0 = t1 - wall
        seg = ev[bisect.bisect_left(times, t0):bisect.bisect_right(times, t1)]
        print('\n=== frame ending at %.3f s (timeline), wall %.1f ms' % (t1 / 1e6, wall / 1000))
        if zero is not None:
            print('    qpc %.1f .. %.1f ms' % (zero + t0 / 1000, zero + t1 / 1000))
        # Render thread.
        mine = [e for e in seg if e[1] == rt]
        waits = collections.Counter()
        for e in mine:
            if e[2] == 'Y':
                waits['syscall ' + SC.get(e[3], str(e[3]))] += e[4]
            elif e[2] == 'H':
                waits['hle 0x%08X' % e[3]] += e[4]
            elif e[2] == 'S':
                waits['pacer sleep'] += e[4]
            elif e[2] == 'Q':
                waits['spin wait'] += e[4]
        blocked = sum(waits.values())
        print('  render thread: cpu %.1f ms, recorded waits %.1f ms, rest %.1f ms'
              % (cpu / 1000, blocked / 1000, (wall - cpu - blocked) / 1000))
        for k, v in waits.most_common(6):
            print('      %-32s %7.1f ms' % (k, v / 1000))
        # Other guest threads' long waits.
        other = collections.defaultdict(collections.Counter)
        for e in seg:
            if e[1] != rt and e[2] in 'YHQS':
                key = {'Y': 'syscall ' + SC.get(e[3], str(e[3])), 'H': 'hle 0x%08X' % e[3],
                       'Q': 'spin wait', 'S': 'sleep'}[e[2]]
                other[who(e[1])][key] += e[4]
        top = sorted(other.items(), key=lambda kv: -sum(kv[1].values()))[:6]
        if top:
            print('  other threads blocked (top):')
            for name, c in top:
                k, v = c.most_common(1)[0]
                print('      %-20s %7.1f ms total, mostly %s %.1f ms' % (name, sum(c.values()) / 1000, k, v / 1000))
        # SPU workloads.
        lanes = collections.defaultdict(lambda: [0, 0, 0])
        for e in seg:
            if e[2] == 'L':
                w = lanes[e[3] >> 8]
                w[0] += 1; w[1] += e[4]; w[2] = max(w[2], e[4])
        if lanes:
            print('  SPU lanes (workload: runs, total, longest):')
            for wid, (n, tot, mx) in sorted(lanes.items(), key=lambda kv: -kv[1][1])[:6]:
                print('      wid%-3d %4d runs %8.1f ms, longest %6.1f ms' % (wid, n, tot / 1000, mx / 1000))
        dispatch = [(e[3], e[4]) for e in seg if e[2] == 'E' and e[4]]
        if dispatch:
            print('  one-SPU dispatch returns: ' + ', '.join('wid%d %.1f ms' % (w, us / 1000) for w, us in dispatch[:8]))
        walks = [(e[3], e[4]) for e in seg if e[2] == 'R']
        if walks:
            print('  FIFO walks: %d, %.1f ms wall (%.1f ms CPU), longest %.1f ms'
                  % (len(walks), sum(w[1] for w in walks) / 1000, sum(w[0] for w in walks) / 1000, max(w[1] for w in walks) / 1000))
        pres = [e[4] for e in seg if e[2] == 'P']
        if pres:
            print('  presents: %d, %.1f ms' % (len(pres), sum(pres) / 1000))
        jobs = collections.defaultdict(lambda: [0, 0])
        for e in seg:
            if e[2] == 'J':
                jobs[e[3]][0] += 1; jobs[e[3]][1] += e[4]
        print('  builder jobs finished: ' + (', '.join('%s %d (%.0f ms)' % (JOB.get(k, k), n, us / 1000)
                                                    for k, (n, us) in sorted(jobs.items())) or 'none'))
        # Hitch trace rows inside the frame (absolute QPC ms).
        if hitch and zero is not None:
            a, b = zero + t0 / 1000, zero + t1 / 1000
            rows = [r for q, r in hitch[bisect.bisect_left(hq, a):bisect.bisect_right(hq, b)]]
            if rows:
                batch = sum(float(r['batch_ms']) for r in rows)
                gpu = max(float(r['gpu_previous_ms']) for r in rows)
                pend = max(float(r['pending']) for r in rows)
                pso = sum(float(r['pso_ms']) for r in rows)
                print('  renderer: %d replay batches, %.1f ms of replay (pipelines %.1f ms), GPU <= %.1f ms/frame, pending <= %.0f'
                      % (len(rows), batch, pso, gpu, pend))
        # ETW thread states.
        if etw and zero is not None:
            a, b = (zero + t0 / 1000) * 1000, (zero + t1 / 1000) * 1000   # absolute QPC us
            def states(tid):
                c = collections.Counter()
                for s, e_, st, _ in etw.get(tid, []):
                    lo, hi = max(s, a), min(e_, b)
                    if hi > lo:
                        c[st] += hi - lo
                return c
            st = states(rt)
            print('  ETW render thread: ' + ', '.join('%s %.1f ms' % (k, v / 1000) for k, v in st.most_common()))


if __name__ == '__main__':
    main()
