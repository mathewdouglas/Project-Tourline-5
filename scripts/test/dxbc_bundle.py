"""Round trip of scripts/dxbc_bundle.py on synthetic bytes (no game data): format, checksums, name filter,
corrupt-record detection and a cancelled build leaving no partial bundle."""
from pathlib import Path
import os
import struct
import sys
import tempfile
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import dxbc_bundle  # noqa: E402


def main():
    assert zlib.crc32(b'123456789') == 0xCBF43926            # the renderer's table CRC-32 has this check value
    with tempfile.TemporaryDirectory() as tmp:
        cache = Path(tmp)
        blobs = {i * 0x0101010101010101 + 1: os.urandom(100 + i * 37) for i in range(40)}
        for key, data in blobs.items():
            (cache / ('%016x.dxbc' % key)).write_bytes(data)
        (cache / 'code_0000000000000001.dxbc').write_bytes(b'not a source-keyed shader')
        (cache / '0123.dxbc').write_bytes(b'short name')
        report = dxbc_bundle.build(cache)
        assert report['shaders'] == 40, report
        assert dxbc_bundle.check(cache) == (40, 0)
        idx = (cache / 'dxbc-bundle-v1.idx').read_bytes()
        assert len(idx) == 16 + 40 * 24 and idx[:9] == b'GT5PDXBI\x01'
        key, off, n, chk = struct.unpack_from('<QQII', idx, 16)
        assert (cache / 'dxbc-bundle-v1.dat').read_bytes()[off:off + n] == blobs[key] and chk == zlib.crc32(blobs[key])
        dat = bytearray((cache / 'dxbc-bundle-v1.dat').read_bytes()); dat[off] ^= 0xFF
        (cache / 'dxbc-bundle-v1.dat').write_bytes(bytes(dat))
        assert dxbc_bundle.check(cache) == (39, 1)
        calls = [0]
        def cancel():
            calls[0] += 1
            raise KeyboardInterrupt('cancelled')
        before = (cache / 'dxbc-bundle-v1.idx').read_bytes()
        try:
            dxbc_bundle.build(cache, poll=cancel)
            raise AssertionError('cancel ignored')
        except KeyboardInterrupt:
            pass
        assert calls[0] == 1 and (cache / 'dxbc-bundle-v1.idx').read_bytes() == before
        assert not list(cache.glob('*.tmp'))
    print('dxbc bundle: ok')


if __name__ == '__main__':
    main()
