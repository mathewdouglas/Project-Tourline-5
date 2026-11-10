"""Table of per-thread CPU from thread_cpu_poll.py around a time window of a route.

    python scripts/diag/thread_cpu_table.py <poll.csv> <run.err> --window <tag.window.json> --from 28 --to 38 [--bin-ms 500]

Times are seconds after grid arrival (window.json grid_observed_ms). One column per bin, one row per thread
that used at least --min-ms in the window, in % of one logical CPU, with the host priority it had.
Thread names come from the run log, as tl.py reads them.
"""
import argparse
import collections
import json
import re


def names(log):
    host, guest = {}, {}
    for path in (log, log.replace('.err', '.out')):
        try:
            for line in open(path, errors='replace'):
                m = re.search(r'\[THREAD (\d+)\].*hosttid=(\d+)', line)
                if m:
                    host[int(m.group(2))] = 'g' + m.group(1)
                m = re.search(r'\[hostthread\] (\S+) hosttid=(\d+)', line)
                if m:
                    host[int(m.group(2))] = m.group(1)
                m = re.search(r'sys_ppu_thread_create tid=(\d+) name="([^"]*)"(?:.*?prio=(-?\d+))?', line)
                if m:
                    guest['g' + m.group(1)] = m.group(2) + ((' p' + m.group(3)) if m.group(3) else '')
        except OSError:
            pass
    return {tid: (name + ' ' + guest[name] if name in guest else name) for tid, name in host.items()}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('poll'); ap.add_argument('log')
    ap.add_argument('--window', required=True)
    ap.add_argument('--from', dest='start', type=float, required=True)
    ap.add_argument('--to', dest='end', type=float, required=True)
    ap.add_argument('--bin-ms', type=int, default=500)
    ap.add_argument('--min-ms', type=float, default=20)
    a = ap.parse_args()
    grid = json.load(open(a.window, encoding='utf-8-sig'))['grid_observed_ms']
    who = names(a.log)
    bins = int((a.end - a.start) * 1000 / a.bin_ms)
    use = collections.defaultdict(lambda: [0.0] * bins)
    prio = {}
    for line in open(a.poll):
        if line[0] in '#u':
            continue
        t, tid, ms, p, _cpu = line.strip().split(',')
        at = (int(t) - grid) / 1000 - a.start
        k = int(at * 1000 / a.bin_ms)
        if 0 <= k < bins:
            use[int(tid)][k] += float(ms); prio[int(tid)] = int(p)
    rows = sorted(((sum(v), tid) for tid, v in use.items() if sum(v) >= a.min_ms), reverse=True)
    print('%-34s %4s ' % ('thread', 'prio') + ' '.join('%4.1f' % (a.start + i * a.bin_ms / 1000) for i in range(bins)))
    total = [0.0] * bins
    for _, tid in rows:
        v = use[tid]
        for i in range(bins):
            total[i] += v[i]
        print('%-34s %4d ' % (who.get(tid, str(tid))[:34], prio[tid]) + ' '.join('%4.0f' % (100 * x / a.bin_ms) for x in v))
    print('%-34s      ' % 'all listed' + ' '.join('%4.0f' % (100 * x / a.bin_ms) for x in total))


if __name__ == '__main__':
    main()
