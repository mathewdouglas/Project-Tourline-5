"""List GT5's SPURS job images from the executable, without running it.

The title's sound jobs are named in tables of (name pointer, image EA, size)
records in its data, e.g. "socdac3", "socisynth" ... and "socepdideq",
"socepdidist", "socepdirta" (those three give size 0). The runtime
recognises a job image by the FNV-1a-64 fingerprint of its bytes
(spu_workload_fingerprint), with the size the job descriptor gives in
16-byte units. A record's size rounded up to 16 is that size. For a size-0
record the image runs up to the next 0x80-aligned image or job body; its
size is taken as the last non-zero byte rounded up to 16 (checked against
captures: --check).

  spu_job_images.py [--elf input/EMAIN.ELF] [--dump analysis/spu/dump] [--write]

Prints each image with its fingerprint and whether a capture exists.
--write saves missing ones as analysis/spu/dump/spujob_<FP>_<size>.bin,
the same files SPU_DUMP_MISS writes (derived game data: never tracked).
"""
import argparse
import os
from pathlib import Path
import struct

root = Path(__file__).resolve().parents[1]
ap = argparse.ArgumentParser()
ap.add_argument("--elf", default=str(root / "input/EMAIN.ELF"))
ap.add_argument("--dump", default=str(root / "analysis/spu/dump"))
ap.add_argument("--write", action="store_true")
a = ap.parse_args()

d = Path(a.elf).read_bytes()
phoff, = struct.unpack_from(">Q", d, 0x20)
n, = struct.unpack_from(">H", d, 0x38)
segs = []
for i in range(n):
    t, _, off, va, _, fs, _, _ = struct.unpack_from(">IIQQQQQQ", d, phoff + 56 * i)
    if t == 1 and fs:
        segs.append((off, va, fs))


def va2off(va):
    for off, v, fs in segs:
        if v <= va < v + fs:
            return off + va - v
    return None


def cstr(va):
    o = va2off(va)
    if o is None:
        return None
    e = d.find(b"\0", o)
    s = d[o:e]
    return s.decode() if 3 <= len(s) < 40 and all(32 < c < 127 for c in s) else None


def fnv(b):
    h = 1469598103934665603
    for c in b:
        h = ((h ^ c) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


# Records: (u32 name -> "soc*" string, u32 ea inside a loaded segment, u32 size).
recs = []
for off, va, fs in segs:
    for o in range(off, off + fs - 12, 4):
        p, ea, size = struct.unpack_from(">III", d, o)
        if not (0x10000 <= p < 0x4000000) or va2off(ea) is None or ea & 0x7F:
            continue
        nm = cstr(p)
        if nm and nm.startswith("soc") and size < 0x100000:
            recs.append((nm, ea, size))
recs = sorted(set(recs), key=lambda r: r[1])
have = {}
for f in Path(a.dump).glob("spujob_*.bin"):
    fp, sz = f.stem.split("_")[1:3]
    have[int(fp, 16)] = int(sz)
def bodies_after(ea):
    """16-byte-aligned job body headers (0xC0DEC0DE at +0x20) after ea."""
    out = []
    for off, va, fs in segs:
        p = d.find(b"\xc0\xde\xc0\xde", va2off(ea) if off <= va2off(ea) < off + fs else off, off + fs)
        while p >= 0:
            h = p - 0x20
            if h % 16 == 0 and va + h - off > ea:
                out.append(va + h - off)
                break
            p = d.find(b"\xc0\xde\xc0\xde", p + 1, off + fs)
    return out


missing = 0
for nm, ea, size in recs:
    o = va2off(ea)
    if size:
        n16 = (size + 15) & ~15
        how = "table"
    else:
        # No size in the record. Measured on captures (01.04 socepdirta,
        # socepdideq; 2.11 socepdideq): the image ends 0x10-0x70 bytes before
        # the next 0x80-aligned image or job body, so try those 8 sizes.
        nxt = min([r[1] for r in recs if r[1] > ea] + bodies_after(ea))
        gap = nxt - ea
        n16, fp = None, None
        for c in range(gap, gap - 0x80, -16):
            f = fnv(d[o:o + c])
            if f in have:
                n16, fp = c, f
                break
        how = "gap"
        if n16 is None:
            print(f"  {nm:14} ea=0x{ea:08X} size=?      (gap {gap:#x}: one of 8, set at run time) NEW, needs SPU_DUMP_MISS")
            missing += 1
            continue
    img = d[o:o + n16]
    fp = fnv(img)
    ok = fp in have
    print(f"  {nm:14} ea=0x{ea:08X} size={n16:6} ({how:7}) fp={fp:016X} {'captured' if ok else 'NEW'}")
    if not ok:
        missing += 1
        if a.write:
            out = Path(a.dump) / f"spujob_{fp:016X}_{n16}.bin"
            out.write_bytes(img)
print(f"{len(recs)} job images, {missing} not captured{' (written)' if a.write and missing else ''}")
