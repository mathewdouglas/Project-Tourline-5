"""Write the renderer's shader bundle (RSX_DXBC_BUNDLE, Phase 237) from a shader cache directory.

    python scripts/dxbc_bundle.py <shadercache dir> [--check]

The renderer reads a source-keyed shader (<16 hex digits>.dxbc) with one positioned read from
dxbc-bundle-v1.dat instead of opening its own file: 5-6 ms an open in a directory of 17,000-57,000 files,
on the thread that records or replays the frame. The game builds the bundle itself in the background, but a
first launch after preparation would race that import; preparation writes it at the end instead.

Format (little-endian), shared with libs/video/rsx_d3d12_backend.c:
  dxbc-bundle-v1.dat  16-byte magic "GT5PDXBD\\1" (zero padded), then the bytecode, appended.
  dxbc-bundle-v1.idx  16-byte magic "GT5PDXBI\\1", then 24-byte records: u64 key, u64 offset in .dat,
                      u32 size, u32 CRC-32 (zlib) of the bytes.
The per-shader files stay; the renderer checks each record's checksum and falls back to the file. The bundle
contains game-derived bytecode, like the cache itself: never ship or commit it.
"""
import argparse
import os
from pathlib import Path
import re
import struct
import zlib

DAT_MAGIC = b'GT5PDXBD\x01' + bytes(7)
IDX_MAGIC = b'GT5PDXBI\x01' + bytes(7)
RECORD = struct.Struct('<QQII')
NAME = re.compile(r'^[0-9a-f]{16}\.dxbc$')
MAX_SHADER = 64 << 20
MAX_RECORDS = 262144 // 4 * 3          # the renderer's table is 3/4 full at most


def checksum(data):
    return zlib.crc32(data) & 0xFFFFFFFF


def build(cache, poll=None):
    """Write <cache>/dxbc-bundle-v1.{dat,idx} for every source-keyed shader file. Returns a small report."""
    cache = Path(cache)
    if not cache.is_dir() or cache.is_symlink():
        raise ValueError('Shader cache must be an ordinary directory')
    names = sorted(e.name for e in os.scandir(cache) if e.is_file(follow_symlinks=False) and NAME.match(e.name))
    if len(names) > MAX_RECORDS:
        names = names[:MAX_RECORDS]
    dat, idx = cache / 'dxbc-bundle-v1.dat', cache / 'dxbc-bundle-v1.idx'
    tmp_dat, tmp_idx = dat.with_suffix('.dat.tmp'), idx.with_suffix('.idx.tmp')
    count = size = 0
    try:
        with tmp_dat.open('wb') as d, tmp_idx.open('wb') as i:
            d.write(DAT_MAGIC); i.write(IDX_MAGIC)
            offset = len(DAT_MAGIC)
            for n, name in enumerate(names):
                if poll and not n % 256:
                    poll()
                data = (cache / name).read_bytes()
                if not data or len(data) > MAX_SHADER:
                    continue
                d.write(data)
                i.write(RECORD.pack(int(name[:16], 16), offset, len(data), checksum(data)))
                offset += len(data); count += 1; size += len(data)
        # Index last: a reader that finds the new index finds all of its data.
        os.replace(tmp_dat, dat); os.replace(tmp_idx, idx)
    finally:
        for t in (tmp_dat, tmp_idx):
            if t.exists():
                t.unlink()
    return dict(status='complete', shaders=count, bytes=size)


def check(cache):
    """Read the bundle back the way the renderer does; returns (records, bad)."""
    cache = Path(cache)
    dat = (cache / 'dxbc-bundle-v1.dat').read_bytes()
    idx = (cache / 'dxbc-bundle-v1.idx').read_bytes()
    if dat[:16] != DAT_MAGIC or idx[:16] != IDX_MAGIC:
        raise ValueError('Bad bundle magic')
    good = bad = 0
    for at in range(16, len(idx) - RECORD.size + 1, RECORD.size):
        key, off, n, chk = RECORD.unpack_from(idx, at)
        body = dat[off:off + n]
        source = cache / ('%016x.dxbc' % key)
        if len(body) != n or checksum(body) != chk or (source.is_file() and source.read_bytes() != body):
            bad += 1
        else:
            good += 1
    return good, bad


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('cache', type=Path)
    ap.add_argument('--check', action='store_true', help='only read an existing bundle back')
    a = ap.parse_args()
    if not a.check:
        print(build(a.cache), flush=True)
    good, bad = check(a.cache)
    print(f'records {good} good, {bad} bad', flush=True)
    raise SystemExit(1 if bad else 0)


if __name__ == '__main__':
    main()
