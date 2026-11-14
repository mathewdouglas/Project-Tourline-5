"""Virtual install: keep installed copies of GT.VOL entries as range records.

GT5's first-boot install copies entries of the disc's GT.VOL into
dev_hdd0/game/BCUS98114/USRDIR/PDIPFS byte for byte. Measured on a 01.04
install: 15,226 of 16,450 files (6.24 GiB) are exact copies of a GT.VOL range
starting on a 0x800 boundary. This tool replaces each such copy with a
256-byte record (layout in the runtime's libs/filesystem/volmap.h); the
runtime's cellFs then serves the file from the volume (PS3_FS_VOLMAP).

No keys and no table of contents: candidates are found by matching each
file's first 64 bytes against every 0x800-aligned position of the volume, and
every candidate is verified in full before its file is replaced. Files that
match nothing (the update package's own files, patched files, the database)
stay as they are.

  volmap.py convert <PDIPFS dir> <GT.VOL> [--guest /dev_bdvd/PS3_GAME/USRDIR/GT.VOL] [--dry-run]
  volmap.py restore <PDIPFS dir> <GT.VOL>   turn every record back into its file
  volmap.py verify  <PDIPFS dir> <GT.VOL>   check every record's range in full
  volmap.py stats   <PDIPFS dir>

Run it only while the game is not running. Each replacement is atomic
(write "<file>.vmtmp", then rename over the file), so an interrupted run
leaves every file either converted or untouched; run it again to continue.
"""
import argparse
import os
from pathlib import Path
import struct
import sys
import time
import zlib

MAGIC = b"PT5VMAP1"
RECORD_SIZE = 256
PATH_MAX = 176
ALIGN = 0x800
KEY = 64
CHUNK = 64 << 20
DEFAULT_GUEST = "/dev_bdvd/PS3_GAME/USRDIR/GT.VOL"


def encode(offset, length, crc, head, tail, volume_size, guest):
    g = guest.encode("ascii")
    if not g.startswith(b"/") or len(g) >= PATH_MAX:
        raise ValueError(f"bad guest volume path: {guest}")
    rec = MAGIC + struct.pack("<IIQQQ", 1, RECORD_SIZE, offset, length, crc)
    rec += head.ljust(16, b"\0") + tail.ljust(16, b"\0")
    rec += struct.pack("<Q", volume_size) + g
    return rec.ljust(RECORD_SIZE, b"\0")


def decode(data):
    if len(data) != RECORD_SIZE or data[:8] != MAGIC:
        return None
    version, size, offset, length, crc = struct.unpack_from("<IIQQQ", data, 8)
    if version != 1 or size != RECORD_SIZE or data[-1] != 0:
        raise ValueError("malformed volume record")
    volume_size, = struct.unpack_from("<Q", data, 72)
    guest = data[80:].split(b"\0", 1)[0].decode("ascii")
    return dict(offset=offset, length=length, crc=crc & 0xFFFFFFFF, head=data[40:56],
                tail=data[56:72], volume_size=volume_size, guest=guest)


def read_record(path):
    try:
        if path.stat().st_size != RECORD_SIZE:
            return None
        return decode(path.read_bytes())
    except OSError:
        return None


def files_under(root):
    for dirpath, _, names in os.walk(root):
        for name in names:
            if not name.endswith(".vmtmp"):
                yield Path(dirpath) / name


def replace_atomically(path, data):
    tmp = path.with_name(path.name + ".vmtmp")
    with open(tmp, "wb") as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


def same_bytes(path, vol, offset, length):
    """Full comparison of a file with a volume range; returns its CRC-32 or None."""
    crc = 0
    with open(path, "rb") as f:
        vol.seek(offset)
        left = length
        while left:
            k = min(left, 8 << 20)
            a = f.read(k)
            if a != vol.read(k) or len(a) != k:
                return None
            crc = zlib.crc32(a, crc)
            left -= k
    return crc


def convert(root, volume, guest, dry_run, min_size):
    t0 = time.time()
    vsize = volume.stat().st_size
    by_key = {}
    kept = 0
    for path in files_under(root):
        size = path.stat().st_size
        if size < max(min_size, KEY) or read_record(path) is not None:
            kept += 1
            continue
        with open(path, "rb") as f:
            key = f.read(KEY)
        by_key.setdefault(key, []).append((path, size))
    pending = sum(len(v) for v in by_key.values())
    print(f"{pending} candidate files, {kept} left as they are (small or already records)")

    done = 0
    saved = 0
    with open(volume, "rb") as vol:
        scan = open(volume, "rb")
        pos = 0
        while pos < vsize and by_key:
            scan.seek(pos)
            block = scan.read(CHUNK + KEY)
            if len(block) < KEY:
                break
            limit = min(CHUNK, len(block) - KEY + 1)
            for i in range(0, limit, ALIGN):
                hits = by_key.get(block[i:i + KEY])
                if not hits:
                    continue
                offset = pos + i
                for entry in list(hits):
                    path, size = entry
                    if offset + size > vsize:
                        continue
                    crc = same_bytes(path, vol, offset, size)
                    if crc is None:
                        continue
                    vol.seek(offset)
                    head = vol.read(min(16, size))
                    vol.seek(offset + size - min(16, size))
                    tail = vol.read(min(16, size))
                    rec = encode(offset, size, crc, head, tail, vsize, guest)
                    if not dry_run:
                        replace_atomically(path, rec)
                    hits.remove(entry)
                    done += 1
                    saved += size - RECORD_SIZE
                if not hits:
                    del by_key[block[i:i + KEY]]
            pos += CHUNK
            print(f"\r  scanned {min(pos, vsize) / 2**30:6.2f} / {vsize / 2**30:.2f} GiB, {done} files"
                  f" ({saved / 2**30:.2f} GiB) {'would be ' if dry_run else ''}replaced", end="")
        scan.close()
    print()
    unmatched = sum(len(v) for v in by_key.values())
    print(f"{done} files {'would become' if dry_run else 'became'} records, "
          f"{saved / 2**30:.2f} GiB; {unmatched} candidates matched no volume range "
          f"(kept); {time.time() - t0:.0f} s")
    return 0


def restore(root, volume):
    n = bad = 0
    with open(volume, "rb") as vol:
        vsize = vol.seek(0, 2)
        for path in files_under(root):
            rec = read_record(path)
            if rec is None:
                continue
            if rec["volume_size"] != vsize:
                print(f"volume size differs for {path}; left as a record", file=sys.stderr)
                bad += 1
                continue
            vol.seek(rec["offset"])
            data = vol.read(rec["length"])
            if len(data) != rec["length"] or zlib.crc32(data) != rec["crc"]:
                print(f"range check failed for {path}; left as a record", file=sys.stderr)
                bad += 1
                continue
            replace_atomically(path, data)
            n += 1
    print(f"{n} records turned back into files, {bad} failed")
    return 1 if bad else 0


def verify(root, volume):
    n = bad = 0
    with open(volume, "rb") as vol:
        vsize = vol.seek(0, 2)
        for path in files_under(root):
            rec = read_record(path)
            if rec is None:
                continue
            n += 1
            crc = 0
            vol.seek(rec["offset"])
            left = rec["length"]
            while left:
                k = min(left, 8 << 20)
                crc = zlib.crc32(vol.read(k), crc)
                left -= k
            if rec["volume_size"] != vsize or crc != rec["crc"]:
                print(f"MISMATCH {path}", file=sys.stderr)
                bad += 1
    print(f"{n} records checked, {bad} mismatches")
    return 1 if bad else 0


def stats(root):
    records = files = rec_bytes = file_bytes = 0
    for path in files_under(root):
        rec = read_record(path)
        if rec is None:
            files += 1
            file_bytes += path.stat().st_size
        else:
            records += 1
            rec_bytes += rec["length"]
    print(f"{records} records standing for {rec_bytes / 2**30:.2f} GiB; "
          f"{files} ordinary files, {file_bytes / 2**30:.2f} GiB on disk")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=["convert", "restore", "verify", "stats"])
    ap.add_argument("root", type=Path, help="the installed PDIPFS folder")
    ap.add_argument("volume", type=Path, nargs="?", help="the disc's GT.VOL")
    ap.add_argument("--guest", default=DEFAULT_GUEST, help="the volume's path as the game opens it")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--min-size", type=int, default=4096,
                    help="leave smaller files as they are (default 4096)")
    a = ap.parse_args()
    if not a.root.is_dir():
        raise SystemExit(f"not a folder: {a.root}")
    if a.command == "stats":
        return stats(a.root)
    if a.volume is None or not a.volume.is_file():
        raise SystemExit("the volume (GT.VOL) is required")
    if a.command == "convert":
        return convert(a.root, a.volume, a.guest, a.dry_run, a.min_size)
    if a.command == "restore":
        return restore(a.root, a.volume)
    return verify(a.root, a.volume)


if __name__ == "__main__":
    sys.exit(main())
