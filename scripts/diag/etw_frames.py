"""Where the worst frames' time went, from Windows' own scheduler record.

    python scripts/diag/etw_frames.py <etw_threads.txt> <timeline.csv> <run.err> <pid>
                                      [--top N] [--after-ms T] [--offset-us X]

<etw_threads.txt> is a WPR CPU-profile trace dumped for the kernel thread
provider only (context switches, ready-thread and thread-name events):

    xperf -i run.etl -o etw_threads.txt -a dumper -range <t0> <t1>
          -provider {3d6fa8d1-fe05-11d0-9dda-00c04fd7ba7c}

The timeline (PS3_TIMELINE, with "#qpc_zero_ms") gives the title's render-thread
frames. ETW timestamps count from the trace's start, so the two clocks are
aligned by matching the render thread's long syscall waits (timeline Y events:
exact end and duration) to the ends of its ETW waits (ready-thread events);
--offset-us skips the search.

For each of the N longest frames this prints, for the render thread, the time
it was running, ready (runnable, waiting for a CPU) and waiting (blocked, by
wait reason); which threads held the CPUs it could use while it was ready;
and the same running/ready/waiting split for the SPU lanes, the renderer
threads and the builder threads.
"""
import argparse
import bisect
import collections
import re
from array import array

THREAD_PROVIDER_NOTE = '{3d6fa8d1-fe05-11d0-9dda-00c04fd7ba7c}'


def proc_of(field):
    m = re.match(r'\s*(.*?)\s*\(\s*(\d+)\)', field)
    return (m.group(1), int(m.group(2))) if m else (field.strip(), -1)


def load_timeline(path):
    zero, ev = None, []
    for line in open(path, errors='replace'):
        if line.startswith('#'):
            m = re.match(r'#qpc_zero_ms ([\d.]+)', line)
            if m:
                zero = float(m.group(1))
            continue
        us, tid, k, a, b = line.strip().split(',')
        ev.append((int(us), int(tid), k, int(a), int(b)))
    ev.sort()
    return zero, ev


def guest_names(log, timeline):
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
    for line in open(timeline, errors='replace'):
        m = re.match(r'#thread (\d+) (.*)', line)
        if m and int(m.group(1)) not in names and m.group(2).strip() != '?':
            names[int(m.group(1))] = m.group(2).strip()
    return {tid: gname.get(n, n) for tid, n in names.items()}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('etw')
    ap.add_argument('timeline')
    ap.add_argument('log')
    ap.add_argument('pid', type=int)
    ap.add_argument('--top', type=int, default=8)
    ap.add_argument('--after-ms', type=float, default=0)
    ap.add_argument('--offset-us', type=float)
    args = ap.parse_args()

    zero, tl = load_timeline(args.timeline)
    frames = [e for e in tl if e[2] == 'C']
    rt = frames[0][1]
    names = guest_names(args.log, args.timeline)

    # One pass over the dump. Per CPU: switch-in times and the thread that ran
    # (a CPU runs that thread until its next switch). For the game's threads:
    # switch-outs (with state and wait reason), ready events and switch-ins.
    cpu_t = collections.defaultdict(lambda: array('q'))
    cpu_tid = collections.defaultdict(lambda: array('q'))
    proc = {}                                       # tid -> process name
    tname = {}                                      # tid -> ETW thread name (SetThreadDescription)
    out_ev = collections.defaultdict(list)          # tid -> [(t, state, reason)]
    in_ev = collections.defaultdict(list)           # tid -> [(t, cpu)]
    rdy_ev = collections.defaultdict(list)          # tid -> [(t, readier tid)]
    with open(args.etw, errors='replace') as f:
        for line in f:
            s = line.lstrip()
            if s.startswith('CSwitch,'):
                p = [x.strip() for x in s.split(',')]
                if not p[1].isdigit():
                    continue
                t, cpu = int(p[1]), int(p[16])
                npn, npid = proc_of(p[2]); ntid = int(p[3])
                opn, opid = proc_of(p[8]); otid = int(p[9])
                proc[ntid] = npn; proc[otid] = opn
                cpu_t[cpu].append(t); cpu_tid[cpu].append(ntid)
                if npid == args.pid:
                    in_ev[ntid].append((t, cpu))
                if opid == args.pid:
                    out_ev[otid].append((t, p[12], p[13]))
            elif s.startswith('ReadyThread,'):
                p = [x.strip() for x in s.split(',')]
                if not p[1].isdigit():
                    continue
                rpn, rpid = proc_of(p[4])
                if rpid == args.pid:
                    rdy_ev[int(p[5])].append((int(p[1]), int(p[3])))
            elif s.startswith('ThreadName,'):
                p = [x.strip() for x in s.split(',', 4)]
                if len(p) > 4 and p[1].isdigit():
                    pn, pid = proc_of(p[2])
                    if pid == args.pid and p[4]:
                        tname[int(p[3])] = p[4].strip('"')

    def label(tid):
        if tid in names:
            return names[tid]
        if tid in tname:
            return tname[tid]
        return '%s:%d' % (proc.get(tid, '?'), tid)

    # Per game thread: a list of (start, end, state, detail) intervals.
    def intervals(tid):
        evs = [(t, 0, c) for t, c in in_ev.get(tid, [])] + \
              [(t, 1, (st, rs)) for t, st, rs in out_ev.get(tid, [])] + \
              [(t, 2, r) for t, r in rdy_ev.get(tid, [])]
        evs.sort(key=lambda e: (e[0], e[1]))
        out, cur, since, detail = [], None, None, ''
        for t, k, x in evs:
            if cur is not None and t > since:
                out.append((since, t, cur, detail))
            if k == 0:
                cur, since, detail = 'running', t, 'cpu%d' % x
            elif k == 1:
                st, rs = x
                cur, since = ('waiting', t) if st == 'Waiting' else ('ready', t)
                detail = rs if st == 'Waiting' else 'preempted'
            else:
                if cur != 'running':
                    cur, since, detail = 'ready', t, 'woken by %s' % label(x)
        return out

    rt_iv = intervals(rt)
    if not rt_iv:
        raise SystemExit('render thread %d has no context switches in the dump' % rt)

    # Clock alignment: timeline long waits on the render thread end at
    # zero + us; ETW waits end at the ready event. offset = qpc_us - etw_us.
    if args.offset_us is not None:
        offset = args.offset_us
    else:
        tl_waits = [(zero * 1000 + e[0], e[4]) for e in tl if e[1] == rt and e[2] == 'Y' and e[4] > 15000]
        etw_waits = [(s, e - s) for s, e, st, d in rt_iv if st == 'waiting' and e - s > 15000]
        votes = collections.Counter()
        for te, td in tl_waits:
            for ee, ed in [(s + d, d) for s, d in etw_waits]:
                if abs(td - ed) < 1500:
                    votes[round((te - ee) / 2000)] += 1   # 2 ms buckets
        if not votes:
            raise SystemExit('could not align clocks: no matching long waits; pass --offset-us')
        bucket, n = votes.most_common(1)[0]
        cand = [te - ee for te, td in tl_waits for ee, ed in [(s + d, d) for s, d in etw_waits]
                if abs(td - ed) < 1500 and round((te - ee) / 2000) == bucket]
        cand.sort()
        offset = cand[len(cand) // 2]
        print('clock alignment: %d of %d long waits agree; offset %.0f us (ETW = qpc_us - offset)'
              % (n, len(tl_waits), offset))

    def to_etw(tl_us):
        return zero * 1000 + tl_us - offset

    # CPUs the render thread ever ran on (its affinity, in practice).
    rt_cpus = sorted({int(d[3:]) for s, e, st, d in rt_iv if st == 'running'})
    print('render thread %s (tid %d) ran on CPUs %s' % (label(rt), rt, rt_cpus))

    def ran_on(cpu, a, b):
        """(tid, us) of every thread that ran on one CPU between a and b."""
        ts, tids = cpu_t[cpu], cpu_tid[cpu]
        i = max(bisect.bisect_right(ts, a) - 1, 0)
        out = collections.Counter()
        while i < len(ts) and ts[i] < b:
            s = max(ts[i], a)
            e = min(ts[i + 1] if i + 1 < len(ts) else b, b)
            if e > s:
                out[tids[i]] += e - s
            i += 1
        return out

    def split(iv, a, b):
        c = collections.Counter()
        for s, e, st, d in iv:
            lo, hi = max(s, a), min(e, b)
            if hi > lo:
                key = st if st != 'waiting' else 'waiting: ' + d
                c[key] += hi - lo
        return c

    groups = collections.defaultdict(list)
    for tid in set(in_ev) | set(out_ev):
        n = label(tid)
        if n.startswith('spurs-lane'):
            groups['SPU lanes'].append(tid)
        elif n in ('shader-compile', 'pso-worker', 'pso-demand', 'interp-variants'):
            groups['builders (' + n + ')'].append(tid)
        elif n in ('present-replay', 'present', 'spurs-jc', 'spurs-kernel'):
            groups[n].append(tid)
    giv = {tid: intervals(tid) for g in groups.values() for tid in g}

    picked = sorted((f for f in frames if f[0] >= args.after_ms * 1000), key=lambda f: -f[4])[:args.top]
    for f in picked:
        t1 = f[0]; t0 = t1 - f[4]
        a, b = to_etw(t0), to_etw(t1)
        print('\n=== frame ending at %.3f s (timeline), %.1f ms; ETW %.3f..%.3f s' % (t1 / 1e6, f[4] / 1000, a / 1e6, b / 1e6))
        st = split(rt_iv, a, b)
        print('  render thread: ' + ', '.join('%s %.1f ms' % (k, v / 1000) for k, v in st.most_common()))
        # Who held the render thread's CPUs while it was ready.
        held = collections.Counter()
        for s, e, stt, d in rt_iv:
            if stt != 'ready':
                continue
            lo, hi = max(s, a), min(e, b)
            if hi <= lo:
                continue
            for cpu in rt_cpus:
                for tid, us in ran_on(cpu, lo, hi).items():
                    held[label(tid)] += us
        if held:
            print('  while it was ready, its CPUs %s ran:' % rt_cpus)
            for n, us in held.most_common(8):
                print('      %-28s %7.1f ms (CPU time summed over those CPUs)' % (n, us / 1000))
        for g, tids in sorted(groups.items()):
            c = collections.Counter()
            for tid in tids:
                c.update(split(giv[tid], a, b))
            tot = sum(c.values())
            if tot:
                print('  %-26s (%d threads): ' % (g, len(tids)) +
                      ', '.join('%s %.0f' % (k, v / 1000) for k, v in c.most_common(4)) + ' ms')


if __name__ == '__main__':
    main()
