"""Check how GT5's scene draws form clip positions (DLSS step 2).

Reads the CSV that RSX_SCENE_CONSTS=<file> writes (one row per depth-tested
draw into the scene target, with c[0..7]) and tests the hypothesis from
docs/plan-dlss.md:

  c[0..3] rows = object to clip (C), c[4..6] rows = object to world (M, with
  a last row 0 0 0 1), so C * inverse(M) is one view-projection per viewport,
  the same for every object drawn through it.

For each frame: the distinct objects (distinct c[0..3]), their view-projection,
and its largest relative difference from the view-projection of the
viewport's object with the most draws. Also how many objects of a frame have
an object of the frame before with the same world matrix (static) or a near
one (moved), which is what per-object motion vectors need.

    scene_consts.py <csv> [--tol 1e-4] [--rows]
"""
import argparse
import csv
from collections import defaultdict



# 4x4 matrices as lists of rows; no numpy in the repo's environment.
def mat_mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def transpose(a):
    return [list(r) for r in zip(*a)]


def det_inv(m):
    """Determinant and inverse (Gauss-Jordan with partial pivoting)."""
    a = [list(map(float, r)) + [1.0 if i == j else 0.0 for j in range(4)] for i, r in enumerate(m)]
    det = 1.0
    for c in range(4):
        p = max(range(c, 4), key=lambda r: abs(a[r][c]))
        if abs(a[p][c]) < 1e-12:
            return 0.0, None
        if p != c:
            a[c], a[p] = a[p], a[c]
            det = -det
        det *= a[c][c]
        pv = a[c][c]
        a[c] = [v / pv for v in a[c]]
        for r in range(4):
            if r != c and a[r][c]:
                f = a[r][c]
                a[r] = [x - f * y for x, y in zip(a[r], a[c])]
    return det, [r[4:] for r in a]


def maxabs(m):
    return max(abs(v) for r in m for v in r)


def diff(a, b):
    return [[x - y for x, y in zip(ra, rb)] for ra, rb in zip(a, b)]


def load(path):
    with open(path, newline='') as f:
        rows = list(csv.DictReader(f))
    out = []
    for r in rows:
        c = [[float(r[f'c{i}_{j}']) for j in range(4)] for i in range(8)]
        out.append(dict(frame=int(r['frame']), record=int(r['record']), verts=int(r['verts']),
                        fp=r['fp'], vs=int(r['vs_idx']), vps=int(r['vp_start']),
                        vp=(int(r['vp_x']), int(r['vp_y']), int(r['vp_w']), int(r['vp_h'])),
                        vt=tuple(float(r[f'vt{i}']) for i in range(4)), c=c))
    return out


def rel(a, b):
    return maxabs(diff(a, b)) / max(maxabs(b), 1e-12)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('csv')
    ap.add_argument('--tol', type=float, default=1e-4)
    ap.add_argument('--rows', action='store_true', help='print every object')
    a = ap.parse_args()
    rows = load(a.csv)
    frames = defaultdict(list)
    for r in rows:
        frames[r['frame']].append(r)
    prev_objs = None
    for fno in sorted(frames):
        draws = frames[fno]
        objs = {}
        for r in draws:
            key = tuple(map(tuple, r['c'][0:4]))
            o = objs.setdefault(key, dict(C=r['c'][0:4], M=r['c'][4:7], vp=r['vp'], draws=0, verts=0,
                                          first=r['record'], fps=set(), vss=set()))
            o['draws'] += 1; o['verts'] += r['verts']; o['fps'].add(r['fp']); o['vss'].add(r['vps'])
        # View-projection per object, for both row and column readings of the matrices.
        res = {}
        for mode in ('rows', 'columns'):
            per_vp = defaultdict(list)
            bad = 0
            for o in objs.values():
                M4 = o['M'] + [[0.0, 0.0, 0.0, 1.0]]
                C = o['C'] if mode == 'rows' else transpose(o['C'])
                if mode != 'rows':
                    M4 = transpose(M4)
                d, inv = det_inv(M4)
                if inv is None or abs(d) < 1e-12:
                    bad += 1
                    continue
                per_vp[o['vp']].append((o['draws'], mat_mul(C, inv), o))
            worst = []
            agree = total = 0
            for vp, lst in per_vp.items():
                ref = max(lst, key=lambda t: t[0])[1]
                for n, VP, o in lst:
                    d = rel(VP, ref)
                    worst.append((d, n, o, vp))
                    total += n
                    if d <= a.tol:
                        agree += n
            res[mode] = (agree, total, bad, per_vp, sorted(worst, key=lambda t: -t[0]))
        print(f'frame {fno}: {len(draws)} draws, {len(objs)} objects, viewports '
              f'{sorted({o["vp"] for o in objs.values()})}')
        for mode, (agree, total, bad, per_vp, worst) in res.items():
            print(f'  {mode}: {agree}/{total} draws share their viewport\'s view-projection to {a.tol:g}; '
                  f'{bad} objects with a singular c[4..6]')
            for d, n, o, vp in worst[:5]:
                print(f'    off by {d:.2e}: {n} draws, {o["verts"]} verts, first record {o["first"]}, vp {vp}, '
                      f'fp {sorted(o["fps"])[:3]}, vp_start {sorted(o["vss"])[:4]}')
        if a.rows:
            for o in sorted(objs.values(), key=lambda o: o['first']):
                print('   ', o['first'], o['draws'], o['vp'], [round(r[3], 3) for r in o['M']])
        if prev_objs is not None:
            same = near = none = 0
            pm = [p['M'] for p in prev_objs.values()]
            for o in objs.values():
                d = min((maxabs(diff(o['M'], m)) for m in pm), default=1e9)
                if d < 1e-6:
                    same += 1
                elif d < 4.0:
                    near += 1
                else:
                    none += 1
            print(f'  against the frame before: {same} objects with the same world matrix, '
                  f'{near} within 4 units, {none} with no near one')
        prev_objs = objs


if __name__ == '__main__':
    main()
