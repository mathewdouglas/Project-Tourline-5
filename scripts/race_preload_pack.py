"""One sequential file for the race prewarm: inspect, verify or rebuild it.

The renderer writes ``warmup-v1/race-pack-v1.bin`` itself after a race load
whose pack was missing or stale, so this tool is not a required step. It
reads and writes the same layout, for checking a pack against the cache it
was made from and for the equivalence test (``scripts/test/race_prewarm.py``).

The pack is derived data. Recipes, bytecode and driver blobs stay in their
own files; deleting the pack only costs the next race load its single read.
``RSX_RACE_PREWARM_PACK=0`` makes the renderer ignore it.

Little-endian layout:

* 40-byte header: magic ``u64``, version ``u32``, count ``u32``, code_count
  ``u32``, flags ``u32``, index_fnv ``u64``, payload_fnv ``u64``.
* ``count`` records of 32 bytes: hash ``u64``, recipe_off ``u32``, recipe_len
  ``u32``, vs_id ``u32``, ps_id ``u32``, blob_off ``u32``, blob_len ``u32``.
* ``code_count`` records of 16 bytes: hash ``u64``, off ``u32``, len ``u32``.
* Payload. Offsets are from the first payload byte. Each shader, recipe and
  driver blob starts 16-byte aligned; the pad bytes are part of the payload.

``index_fnv`` is FNV-1a 64 over the little-endian hash list in pack order
(index order when written, each hash once). ``payload_fnv`` is the same
function over the payload. A stored driver blob is the raw cached pipeline
without its 24-byte file header. A missing or invalid recipe, shader or blob
is a zero length, so one bad file does not drop the rest of the index; the
renderer prepares such a member from its files and rewrites the pack.

Usage::

    python scripts/race_preload_pack.py check <shadercache>
    python scripts/race_preload_pack.py build <shadercache> [--dest FILE]
"""
import argparse
import struct
from pathlib import Path

MAGIC = 0x314B434150355447  # 'GT5PACK1' little-endian
VERSION = 1
RECIPE_MAGIC = 0x31574D5241503547
BLOB_MAGIC = 0x31534F5035505447
HEADER = struct.Struct('<QIIIIQQ')
RECORD = struct.Struct('<QIIIIII')
CODE = struct.Struct('<QII')
NO_CODE = 0xFFFFFFFF
MAX_MEMBERS = 32768
MAX_BLOB = 32 * 1024 * 1024
MAX_CODE = 4 * 1024 * 1024
MAX_PACK = 512 * 1024 * 1024
_MASK = (1 << 64) - 1


def fnv64(data, value=1469598103934665603):
    """The renderer's FNV-1a 64 (pure Python: about 10 MB a second)."""
    for byte in bytes(data):
        value = ((value ^ byte) * 1099511628211) & _MASK
    return value


def index_hashes(path):
    """Index lines in order, each hash once, as the renderer reads them."""
    hashes, seen = [], set()
    for line in Path(path).read_text(encoding='ascii', errors='replace').splitlines():
        token = line.split()[0] if line.split() else ''
        try:
            value = int(token, 16)
        except ValueError:
            continue
        if not value or value in seen or len(hashes) >= MAX_MEMBERS:
            continue
        seen.add(value)
        hashes.append(value)
    return hashes


def _fnv_hashes(hashes):
    return fnv64(b''.join(struct.pack('<Q', h) for h in hashes))


def _read(path):
    try:
        return path.read_bytes()
    except OSError:
        return b''


def read_recipe(path, expected):
    """A recipe's bytes when its envelope is valid for ``expected``, else b''.

    Checks the magic, version, size, hash and checksum. The renderer also
    checks the pipeline description inside, again when it uses the pack.
    """
    raw = _read(path)
    if len(raw) < 56 or len(raw) % 8:
        return b''
    magic, version, size, pipe = struct.unpack_from('<QIIQ', raw, 0)
    if magic != RECIPE_MAGIC or version != 1 or size != len(raw) or pipe != expected:
        return b''
    if struct.unpack_from('<Q', raw, len(raw) - 8)[0] != fnv64(raw[:-8]):
        return b''
    return raw


def read_blob(cache, hash_):
    raw = _read(Path(cache) / f'pso_{hash_:016x}.bin')
    if len(raw) <= 24 or len(raw) > MAX_BLOB:
        return b''
    magic, size, checksum = struct.unpack_from('<QQQ', raw, 0)
    blob = raw[24:]
    if magic != BLOB_MAGIC or size != len(blob) or fnv64(blob) != checksum:
        return b''
    return blob


def build_bytes(warmup, index_name='race-v1.txt'):
    """The pack for one index, as the renderer would write it. ``warmup`` is ``<shadercache>/warmup-v1``."""
    warmup = Path(warmup)
    cache = warmup.parent
    hashes = index_hashes(warmup / index_name)
    codes, code_rows, records = {}, [], []
    payload = bytearray()

    def align():
        payload.extend(b'\0' * ((-len(payload)) & 15))

    def add_code(hash_, length):
        if not 0 < length <= MAX_CODE:
            return NO_CODE
        raw = _read(warmup / f'code_{hash_:016x}.dxbc')
        if len(raw) != length or fnv64(raw) != hash_:
            return NO_CODE
        key = (hash_, length)
        if key not in codes:
            align()
            codes[key] = len(code_rows)
            code_rows.append((hash_, len(payload), length))
            payload.extend(raw)
        return codes[key]

    for hash_ in hashes:
        recipe = read_recipe(warmup / f'pso_{hash_:016x}.recipe', hash_)
        vs_id = ps_id = NO_CODE
        recipe_off = 0
        if recipe:
            vs_hash, ps_hash, vs_len, ps_len = struct.unpack_from('<QQII', recipe, 24)
            vs_id = add_code(vs_hash, vs_len)
            ps_id = add_code(ps_hash, ps_len)
            align()
            recipe_off = len(payload)
            payload.extend(recipe)
        blob = read_blob(cache, hash_)
        blob_off = 0
        if blob:
            align()
            blob_off = len(payload)
            payload.extend(blob)
        records.append((hash_, recipe_off, len(recipe), vs_id, ps_id, blob_off, len(blob)))

    body = bytearray(HEADER.pack(MAGIC, VERSION, len(hashes), len(code_rows), 0,
                                 _fnv_hashes(hashes), fnv64(payload)))
    for record in records:
        body.extend(RECORD.pack(*record))
    for row in code_rows:
        body.extend(CODE.pack(*row))
    body.extend(payload)
    if not hashes or len(body) > MAX_PACK:
        raise ValueError('the index is empty or the pack would exceed its size limit')
    return bytes(body)


def build(warmup, index_name='race-v1.txt', dest=None):
    warmup = Path(warmup)
    body = build_bytes(warmup, index_name)
    dest = Path(dest) if dest else warmup / 'race-pack-v1.bin'
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_suffix(dest.suffix + '.tmp')
    tmp.write_bytes(body)
    tmp.replace(dest)
    parsed = parse_bytes(body)
    return dict(path=str(dest), recipes=parsed['count'], codes=parsed['code_count'], bytes=len(body),
                blobs=sum(1 for record in parsed['records'] if record[6]))


def parse_bytes(data):
    """Header fields, tables and payload. Raises ValueError when ``data`` is not a sound pack."""
    if len(data) < HEADER.size:
        raise ValueError('pack is shorter than its header')
    magic, version, count, code_count, flags, index_fnv, payload_fnv = HEADER.unpack_from(data, 0)
    if magic != MAGIC or version != VERSION:
        raise ValueError('pack magic or version does not match')
    if not count or count > MAX_MEMBERS or code_count > MAX_MEMBERS * 2:
        raise ValueError('pack counts are out of range')
    tables = HEADER.size + count * RECORD.size + code_count * CODE.size
    if tables > len(data):
        raise ValueError('pack tables run past the file')
    payload = data[tables:]
    if fnv64(payload) != payload_fnv:
        raise ValueError('pack payload checksum does not match')
    records = [RECORD.unpack_from(data, HEADER.size + i * RECORD.size) for i in range(count)]
    codes = [CODE.unpack_from(data, HEADER.size + count * RECORD.size + i * CODE.size) for i in range(code_count)]
    members = [record[0] for record in records]
    if _fnv_hashes(members) != index_fnv:
        raise ValueError('pack index checksum does not match its records')
    if 0 in members or len(set(members)) != len(members):
        raise ValueError('pack lists a member twice or a zero hash')
    for _hash, off, length in codes:
        if off & 15 or off + length > len(payload) or fnv64(payload[off:off + length]) != _hash:
            raise ValueError('pack shader bytes do not match their table entry')
    for hash_, recipe_off, recipe_len, vs_id, ps_id, blob_off, blob_len in records:
        if recipe_off + recipe_len > len(payload) or blob_off + blob_len > len(payload):
            raise ValueError(f'pack member {hash_:016x} points past the payload')
        if (recipe_len and recipe_off & 15) or (blob_len and blob_off & 15):
            raise ValueError(f'pack member {hash_:016x} is not aligned')
        for code_id in (vs_id, ps_id):
            if code_id != NO_CODE and code_id >= code_count:
                raise ValueError(f'pack member {hash_:016x} names a missing shader')
    return dict(count=count, code_count=code_count, flags=flags, index_fnv=index_fnv,
                payload_fnv=payload_fnv, records=records, codes=codes, payload=payload, bytes=len(data))


def parse(path):
    return parse_bytes(Path(path).read_bytes())


def check(warmup, index_name='race-v1.txt'):
    """Compare the stored pack with the index and the files it was made from."""
    warmup = Path(warmup)
    parsed = parse(warmup / 'race-pack-v1.bin')
    members = [record[0] for record in parsed['records']]
    listed = index_hashes(warmup / index_name)
    complete = sum(1 for r in parsed['records'] if r[2] and r[6] and r[3] != NO_CODE and r[4] != NO_CODE)
    return dict(recipes=parsed['count'], codes=parsed['code_count'], bytes=parsed['bytes'], complete=complete,
                index_members=len(listed), matches_index=sorted(members) == sorted(listed),
                same_bytes_as_rebuild=build_bytes(warmup, index_name) == (warmup / 'race-pack-v1.bin').read_bytes())


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('action', choices=('check', 'build'))
    parser.add_argument('cache', type=Path, help='the shader cache directory (holds warmup-v1)')
    parser.add_argument('--dest', type=Path, help='build: write here instead of warmup-v1/race-pack-v1.bin')
    args = parser.parse_args()
    warmup = args.cache / 'warmup-v1'
    report = check(warmup) if args.action == 'check' else build(warmup, dest=args.dest)
    for key, value in report.items():
        print(f'{key}: {value}')


if __name__ == '__main__':
    main()
