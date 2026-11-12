"""How many of a run's (vertex program, fragment program) pairs the disc defines.

A driver pipeline needs both shaders, so an offline pipeline library can only
cover a live pair that some SHDS shader definition names. Disc side: every
SHDS table in the given files or directories (an unpacked GT.VOL, the
executable). Live side: an RSX_FP_CATALOG directory (fp_*.bin, vp_*.bin,
pairs-v1.txt). Vertex programs compare by the renderer's cache hash (FNV-1a
over the stored words up to END); fragment programs by structural identity.

SHDS layout: header +0x16 definition count, +0x18 vertex program count,
+0x1C definitions (0x0C each: +0x08 vertex index, +0x0A fragment index),
+0x20 vertex programs (0x28 each: +0x00 offset, +0x04 size), +0x34 fragment
program count, +0x40 fragment programs (0x20 each: +0x04 offset, +0x08 size).
Offsets are relative to the SHDS header or the enclosing MDL3. Field names
follow Nenkai/PDTools (MIT); the vertex/fragment roles were checked here
against END bits and index ranges.

  .venv\\Scripts\\python scripts\\diag\\pair_coverage.py <live dir> <file|dir>... [--save F] [--load F]
Reads local game-derived files; prints counts only. --save writes the disc
hashes (no program bytes) so the unpacked volume can be deleted.
"""
import hashlib
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from fp_coverage import structure   # noqa: E402

GPU_SALT = 0x6A09E667


def vp_hash(words_le):
    h = 2166136261
    for c in words_le:
        h = ((h ^ c) * 16777619) & 0xffffffff
    return h or 1


def vp_program(be):
    """A vertex record is a header whose word at +0x10 is the microcode's
    offset (0x30 in every record checked). Disc words are big-endian; the
    renderer stores them host order."""
    if len(be) < 0x14: return None
    start = struct.unpack_from('>I', be, 0x10)[0]
    if start % 16 or start >= len(be): return None
    out = bytearray()
    for o in range(start, len(be) - 15, 16):
        ins = b''.join(be[o + i:o + i + 4][::-1] for i in range(0, 16, 4))
        out += ins
        if struct.unpack_from('<I', ins, 12)[0] & 1: return bytes(out)
    return None


def tables(d):
    s = 0
    while True:
        h = d.find(b'SHDS', s)
        if h < 0: return
        s = h + 4
        if h + 0x44 > len(d): continue
        ndef, nvp = struct.unpack_from('>HH', d, h + 0x16)
        tdef, tvp = struct.unpack_from('>II', d, h + 0x1C)
        nfp = struct.unpack_from('>H', d, h + 0x34)[0]
        tfp = struct.unpack_from('>I', d, h + 0x40)[0]
        if not (0 < nvp <= 8192 and 0 < nfp <= 8192 and ndef <= 16384): continue
        model = d.rfind(b'MDL3', 0, h)
        for base in ([model, h] if model >= 0 else [h]):
            try:
                vps = [struct.unpack_from('>II', d, base + tvp + i * 0x28) for i in range(nvp)]
                fps = [struct.unpack_from('>II', d, base + tfp + i * 0x20 + 4) for i in range(nfp)]
                defs = [struct.unpack_from('>HH', d, base + tdef + i * 0x0C + 8) for i in range(ndef)]
            except struct.error:
                continue
            # One executable table has a vertex program of 2002 bytes: sizes
            # need not be whole instructions; the program ends at its END bit.
            if not all(o and z and base + o + z <= len(d) for o, z in vps + fps): continue
            vh = [vp_program(d[base + o:base + o + z]) for o, z in vps]
            fh = [structure(d[base + o:base + o + z]) for o, z in fps]
            yield ([vp_hash(v) if v else None for v in vh], fh,
                   [(a, b) for a, b in defs if a < nvp and b < nfp])
            break


def scan(paths):
    vps, fps, pairs, n = set(), set(), set(), 0
    files = []
    for a in paths:
        p = Path(a)
        files += [q for q in p.rglob('*') if q.is_file()] if p.is_dir() else [p]
    for f in files:
        try: d = f.read_bytes()
        except OSError: continue
        if b'SHDS' not in d: continue
        for vh, fh, defs in tables(d):
            n += 1
            vps.update(v for v in vh if v); fps.update(x for x in fh if x)
            for a, b in defs:
                if vh[a] and fh[b]: pairs.add((vh[a], fh[b]))
    return n, vps, fps, pairs


def main():
    args = sys.argv[1:]
    save = load = None
    if '--save' in args: i = args.index('--save'); save = args[i + 1]; del args[i:i + 2]
    if '--load' in args: i = args.index('--load'); load = args[i + 1]; del args[i:i + 2]
    if not args or (len(args) < 2 and not load): raise SystemExit(__doc__)
    live = Path(args[0])
    n, vps, fps, pairs = scan(args[1:])
    if load:
        j = json.loads(Path(load).read_text())
        vps |= set(j['vp']); fps |= set(j['fp']); pairs |= {tuple(p) for p in j['pairs']}; n += j['tables']
    if save:
        Path(save).write_text(json.dumps(dict(tables=n, vp=sorted(vps), fp=sorted(fps), pairs=sorted(pairs))))
    print(f'disc: {n} SHDS tables, {len(vps)} vertex programs, {len(fps)} fragment structures, {len(pairs)} defined pairs')

    fp_struct = {}
    for p in live.glob('fp_*.bin'):
        s = structure(p.read_bytes())
        if s: fp_struct[p.stem[3:]] = s
    live_vp = {}
    for p in live.glob('vp_*.bin'):
        h = int(p.stem[3:], 16)
        live_vp[h] = h if vp_hash(p.read_bytes()) == h else h ^ GPU_SALT   # gpu-fetch entries are salted
    lp = set()
    for line in (live / 'pairs-v1.txt').read_text().split('\n'):
        if not line.strip(): continue
        v, f = line.split()
        v = live_vp.get(int(v, 16)); s = fp_struct.get(f.zfill(16))
        if v is not None and s: lp.add((v, s))
    lv = {v for v, _ in lp}; lf = {f for _, f in lp}
    pct = lambda a, b: f'{a} of {b} ({100 * a / max(1, b):.1f}%)'
    print('live vertex programs on disc:', pct(len(lv & vps), len(lv)))
    print('live fragment programs on disc:', pct(len(lf & fps), len(lf)))
    print('live pairs, both programs on disc:', pct(sum(v in vps and f in fps for v, f in lp), len(lp)))
    print('live pairs defined together on disc:', pct(len(lp & pairs), len(lp)))


if __name__ == '__main__':
    main()
