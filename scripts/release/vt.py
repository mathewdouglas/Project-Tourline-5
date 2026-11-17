"""VirusTotal checks for release files, and byte-patched variants for finding
what an antivirus engine scores (docs/release-av.md).

    python scripts/release/vt.py scan <file|zip>...       look up, upload if new, wait
    python scripts/release/vt.py scan --rescan <file>...  ask for a fresh analysis
    python scripts/release/vt.py variant <exe> <out.exe> --map <lld map> \
        [--fill-obj REGEX] [--fill-sym REGEX] [--fill-va LO-HI] [--invert]

scan exits 1 when any engine reports a file as malicious, so a release stops
before it is public. A ZIP is checked as uploaded and member by member
(.exe/.dll), because engines can flag a member without flagging the archive.
The API key comes from VT_API_KEY or --key-file and is never printed; it is
sent to curl through a temporary header file, not the command line. The free
API allows 4 requests a minute, so waits poll once a minute.

variant copies an exe, fills the chosen code with int3 (0xCC) and gives the
copy a new PE timestamp, so every variant has a new hash. Regions come from a
linker map (`/MAP`, lld-link format): whole objects (`--fill-obj`, matched
against the map's object column), symbols (`--fill-sym`) or VA ranges.
`--invert` fills all .text except the chosen regions and the entry point's
object. Variants are for scanning only; they do not run.

Uploading sends the file to VirusTotal, which shares it with its vendors:
use it only for files that are about to be public anyway (the player ZIP and
its programs). Never upload game data or anything under input/.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import time
import zipfile

API = 'https://www.virustotal.com/api/v3'
SCANNED = ('.exe', '.dll')
# Detections that do not fail a scan: (engine, exact label). Trapmine's
# low-confidence machine-learning label on tourline5.exe has been there since
# 0.1.10 and nothing else agrees with it (accepted 2026-11-18). Anything else,
# including another label from the same engine, still fails.
ALLOWED = {('Trapmine', 'suspicious.low.ml.score')}


def api_key(args):
    if args.key_file:
        return Path(args.key_file).read_text().strip()
    k = os.environ.get('VT_API_KEY', '').strip()
    if not k:
        sys.exit('vt.py: set VT_API_KEY or pass --key-file')
    return k


def curl(key, args, timeout=120):
    """Run curl with the key in a header file; return parsed JSON (or None)."""
    with tempfile.NamedTemporaryFile('w', suffix='.txt', delete=False) as h:
        h.write('x-apikey: ' + key + '\n')
    try:
        for attempt in range(6):
            r = subprocess.run(['curl', '-sS', '-m', str(timeout), '-H', '@' + h.name] + args,
                               capture_output=True, text=True)
            out = r.stdout.strip()
            if r.returncode == 0 and out:
                d = json.loads(out)
                if d.get('error', {}).get('code') == 'QuotaExceededError':
                    time.sleep(60)
                    continue
                return d
            if r.returncode == 0:
                return None
            time.sleep(15)
        sys.exit(f'vt.py: request failed: {r.stderr.strip()[:200]}')
    finally:
        os.unlink(h.name)


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def report(name, attrs):
    stats = attrs.get('last_analysis_stats') or attrs.get('stats') or {}
    res = attrs.get('last_analysis_results') or attrs.get('results') or {}
    hits = sorted((k, v.get('result')) for k, v in res.items() if v.get('category') == 'malicious')
    bad = [h for h in hits if h not in ALLOWED]
    total = sum(stats.get(k, 0) for k in ('malicious', 'suspicious', 'undetected', 'harmless'))
    print(f'{name}: {len(hits)}/{total} malicious' +
          ''.join(f' {k}={v}' + (' (allowed)' if (k, v) in ALLOWED else '') for k, v in hits), flush=True)
    return bad


def wait(key, analysis_id, name):
    for _ in range(90):
        d = curl(key, [f'{API}/analyses/{analysis_id}'])
        a = d['data']['attributes']
        if a.get('status') == 'completed':
            return a
        time.sleep(60)
    sys.exit(f'vt.py: {name}: analysis did not complete')


def upload(key, path):
    size = Path(path).stat().st_size
    url = f'{API}/files'
    if size > 32 << 20:
        url = curl(key, [f'{API}/files/upload_url'])['data']
    d = curl(key, ['-F', f'file=@{path}', url], timeout=3600)
    return d['data']['id']


def scan_one(key, path, name, rescan):
    h = sha256(path)
    d = curl(key, [f'{API}/files/{h}'])
    known = d and 'data' in d
    if known and not rescan:
        return report(f'{name} ({h[:12]}, known)', d['data']['attributes'])
    if known:
        aid = curl(key, ['-X', 'POST', '-H', 'content-length: 0', f'{API}/files/{h}/analyse'])['data']['id']
    else:
        print(f'{name}: uploading ({Path(path).stat().st_size >> 20} MB)', flush=True)
        aid = upload(key, path)
    return report(f'{name} ({h[:12]})', wait(key, aid, name))


def cmd_scan(args):
    key = api_key(args)
    flagged = []
    for p in args.files:
        p = Path(p)
        if scan_one(key, p, p.name, args.rescan):
            flagged.append(p.name)
        if p.suffix.lower() == '.zip' and not args.archive_only:
            with tempfile.TemporaryDirectory() as td, zipfile.ZipFile(p) as z:
                for m in z.namelist():
                    if m.lower().endswith(SCANNED):
                        out = Path(z.extract(m, td))
                        if scan_one(key, out, f'{p.name}:{m}', args.rescan):
                            flagged.append(f'{p.name}:{m}')
    if flagged:
        print('FLAGGED: ' + ', '.join(flagged))
        return 1
    print('clean')
    return 0


def pe_sections(b):
    pe = struct.unpack_from('<I', b, 0x3C)[0]
    ns = struct.unpack_from('<H', b, pe + 6)[0]
    opt = struct.unpack_from('<H', b, pe + 20)[0]
    base = struct.unpack_from('<Q', b, pe + 24 + 24)[0]
    entry = struct.unpack_from('<I', b, pe + 24 + 16)[0]
    secs = []
    for i in range(ns):
        o = pe + 24 + opt + 40 * i
        name = b[o:o + 8].rstrip(b'\0').decode()
        vs, va, rs, ra = struct.unpack_from('<IIII', b, o + 8)
        secs.append((name, va, vs, ra, rs))
    return pe, base, entry, secs


def map_text(path):
    syms = []
    for line in open(path, encoding='latin-1'):
        m = re.match(r'\s+0001:[0-9a-f]+\s+(\S+)\s+([0-9a-f]{16})\s+(.*)$', line)
        if m:
            syms.append((int(m.group(2), 16), m.group(1), m.group(3).strip()))
    syms.sort()
    return syms


def cmd_variant(args):
    b = bytearray(Path(args.exe).read_bytes())
    pe, base, entry, secs = pe_sections(b)
    text = next(s for s in secs if s[0] == '.text')
    t_lo, t_hi = base + text[1], base + text[1] + min(text[2], text[4])
    ranges = []
    if args.map:
        syms = map_text(args.map)
        ends = [s[0] for s in syms[1:]] + [t_hi]
        entry_obj = next((o for (va, n, o) in syms if va == base + entry), None)
        for (va, name, obj), end in zip(syms, ends):
            hit = ((args.fill_obj and re.search(args.fill_obj, obj)) or
                   (args.fill_sym and re.search(args.fill_sym, name)))
            if args.invert:
                hit = not hit and obj != entry_obj
            if hit and end > va:
                ranges.append((va, end))
    for r in args.fill_va or []:
        lo, hi = (int(x, 16) for x in r.split('-'))
        ranges.append((lo, hi))
    filled = 0
    for lo, hi in ranges:
        lo, hi = max(lo, t_lo), min(hi, t_hi)
        if hi <= lo:
            continue
        off = text[3] + (lo - base - text[1])
        b[off:off + (hi - lo)] = b'\xCC' * (hi - lo)
        filled += hi - lo
    ts = struct.unpack_from('<I', b, pe + 8)[0]
    struct.pack_into('<I', b, pe + 8, (ts + 1 + int(time.time())) & 0xFFFFFFFF)
    Path(args.out).write_bytes(b)
    print(f'{args.out}: filled {filled} bytes in {len(ranges)} ranges, sha256 {sha256(args.out)}')
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('scan')
    s.add_argument('files', nargs='+')
    s.add_argument('--key-file')
    s.add_argument('--rescan', action='store_true', help='request a new analysis of known files')
    s.add_argument('--archive-only', action='store_true', help='do not scan ZIP members separately')
    v = sub.add_parser('variant')
    v.add_argument('exe')
    v.add_argument('out')
    v.add_argument('--map')
    v.add_argument('--fill-obj')
    v.add_argument('--fill-sym')
    v.add_argument('--fill-va', action='append')
    v.add_argument('--invert', action='store_true')
    a = ap.parse_args()
    return cmd_scan(a) if a.cmd == 'scan' else cmd_variant(a)


if __name__ == '__main__':
    sys.exit(main())
