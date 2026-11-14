"""Virtual install: scripts/volmap.py and the runtime's cellFs volume records.

Builds a synthetic disc volume and install (no game data), converts the
install with volmap.py, then compiles the runtime's real cellFs.c and
volmap.c with scripts/test/volmap_harness.c and checks reads, seeks, stat,
copy on write, truncation, refusal of writes, a changed or missing volume,
PS3_FS_VOLMAP=0/2, and volmap.py restore/verify.
"""
import os
from pathlib import Path
import random
import shutil
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
runtime = Path(os.environ.get('PS3RECOMP_DIR', root.parent / 'ps3recomp-tourline5'))
work = Path(tempfile.mkdtemp(prefix='volmap-'))  # system temp: never a worktree folder
py = sys.executable
rng = random.Random(5)


def blob(n):
    return bytes(rng.getrandbits(8) for _ in range(n))


# A volume of 0x800-aligned entries, as GT.VOL lays them out.
entries = [blob(n) for n in (10000, 5000, 7000, 100, 9000)]
vol = bytearray(blob(0x9E800))
offsets = []
for e in entries:
    offsets.append(len(vol))
    vol += e + bytes(-len(e) % 0x800)
volume = work / 'disc/PS3_GAME/USRDIR/GT.VOL'
volume.parent.mkdir(parents=True)
volume.write_bytes(vol)

pfs = work / 'gamedata/dev_hdd0/game/BCUS98114/USRDIR/PDIPFS'
for rel, data in {'A/1': entries[0], 'A/2': entries[1], 'B/3': blob(6000),
                  'A/4': entries[3], 'C/5': entries[4][:8000] + blob(1000)}.items():
    (pfs / rel).parent.mkdir(parents=True, exist_ok=True)
    (pfs / rel).write_bytes(data)
(work / 'expected').mkdir()
(work / 'expected/A1').write_bytes(entries[0])
(work / 'expected/A2').write_bytes(entries[1])
originals = {p: p.read_bytes() for p in pfs.rglob('*') if p.is_file()}


def tool(*args, check=True):
    r = subprocess.run([py, '-I', str(root / 'scripts/volmap.py'), *args],
                       capture_output=True, text=True)
    if check and r.returncode:
        raise SystemExit(f'volmap.py {args[0]} failed:\n{r.stdout}{r.stderr}')
    return r.stdout


out = tool('convert', str(pfs), str(volume))
assert '2 files became records' in out, out
for rel in ('A/1', 'A/2'):
    assert (pfs / rel).stat().st_size == 256, rel
for rel in ('B/3', 'A/4', 'C/5'):   # no match, too small, matching start only
    assert (pfs / rel).read_bytes() == originals[pfs / rel], rel
assert '2 records' in tool('stats', str(pfs))
assert '0 mismatches' in tool('verify', str(pfs), str(volume))
assert '0 files became records' in tool('convert', str(pfs), str(volume))  # idempotent

# restore turns records back; convert again for the runtime checks.
tool('restore', str(pfs), str(volume))
assert all(p.read_bytes() == d for p, d in originals.items())
tool('convert', str(pfs), str(volume))

# Build the harness from the runtime's own sources.
exe = work / 'harness.exe'
inc = [runtime, runtime / 'include', runtime / 'libs/filesystem']
batch = work / 'build.cmd'
batch.write_text(
    '@echo off\nset "PATH=C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer;%PATH%"\n'
    'call "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\n'
    '"C:\\Program Files\\LLVM\\bin\\clang-cl.exe" /nologo /O1 /MD /W3 /D_CRT_SECURE_NO_WARNINGS '
    '-Wno-unused-function -Wno-unused-variable '
    + ' '.join(f'/I"{p}"' for p in inc)
    + f' /Fo"{work}/" /Fe"{exe}" "{root / "scripts/test/volmap_harness.c"}" '
    f'"{runtime / "libs/filesystem/cellFs.c"}" "{runtime / "libs/filesystem/volmap.c"}" dbghelp.lib\n')
subprocess.run(['cmd', '/c', str(batch)], check=True)


def harness(mode, **env):
    e = dict(os.environ, **env)
    e.pop('FS_HIDE', None)
    r = subprocess.run([str(exe), str(work), mode], capture_output=True, text=True, env=e)
    if r.returncode:
        raise SystemExit(f'harness {mode} {env} failed:\n{r.stderr[-3000:]}')


harness('off', PS3_FS_VOLMAP='0')
pristine = volume.read_bytes()
mid = bytearray(pristine)
mid[offsets[0] + 5000] ^= 0xFF
volume.write_bytes(mid)
harness('midbad')                       # head/tail check only: still served
harness('midbad', PS3_FS_VOLMAP='2')    # full check: refused
head = bytearray(pristine)
head[offsets[0]] ^= 0xFF
volume.write_bytes(head)
harness('headbad')
volume.rename(volume.with_suffix('.away'))
harness('nodisc')
volume.with_suffix('.away').rename(volume)
volume.write_bytes(pristine)
harness('serve')                        # last: it turns A/1 and A/2 back into files
assert (pfs / 'A/2').read_bytes() == entries[1]
assert (pfs / 'A/1').read_bytes() == entries[0][:50]
assert not list(pfs.rglob('*.vmtmp'))

shutil.rmtree(work)
print('volmap: converter and runtime record checks passed')
