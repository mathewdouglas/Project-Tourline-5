"""How many of the fragment programs a run used are in an offline SHDS catalogue.

Live programs come from RSX_FP_CATALOG=<dir> (fp_<identity>.bin); catalogues
are scripts/precompile_shaders.py --catalog-only output (*.fp). Both sides are
compared by the renderer's structural identity (rsx_fp_structural_hash): the
16-byte instruction blocks up to the END bit, inline constant slots skipped.
Reads local game-derived files only; prints counts and unmatched live names.

  .venv\\Scripts\\python scripts\\diag\\fp_coverage.py <live dir> <catalogue dir|.fp file>...
"""
import hashlib
import struct
import sys
from pathlib import Path


def word(b, o):
    be = struct.unpack_from('>I', b, o)[0]
    return ((be << 16) | (be >> 16)) & 0xffffffff   # rsx_fp_read_word


def structure(b):
    out = bytearray(); o = 0
    while o + 16 <= len(b):
        w0, w1, w2, w3 = (word(b, o + 4 * i) for i in range(4))
        out += b[o:o + 16]; o += 16
        if any((w & 3) == 2 for w in (w1, w2, w3)): o += 16   # FP_REG_TYPE_CONST: inline slot
        if w0 & 1: return hashlib.sha256(bytes(out)).hexdigest()   # FP_END
    return None


def main():
    if len(sys.argv) < 3: raise SystemExit(__doc__)
    live = {}
    for p in Path(sys.argv[1]).glob('fp_*.bin'):
        s = structure(p.read_bytes())
        if s: live[s] = p.name
    catalogue = set()
    for a in sys.argv[2:]:
        for p in (Path(a).glob('*.fp') if Path(a).is_dir() else [Path(a)]):
            s = structure(p.read_bytes())
            if s: catalogue.add(s)
    hit = sum(1 for s in live if s in catalogue)
    print(f'live {len(live)} distinct, catalogue {len(catalogue)} distinct structures, '
          f'matched {hit} ({100 * hit / max(1, len(live)):.1f}%)')
    for s, name in sorted(live.items(), key=lambda kv: kv[1]):
        if s not in catalogue: print('  unmatched', name)


if __name__ == '__main__':
    main()
