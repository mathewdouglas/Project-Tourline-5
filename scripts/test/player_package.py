"""Check player staging/ZIP output and refusal paths without game/dependency binaries."""
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import subprocess
import tempfile
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('player_package', ROOT / 'scripts/release/package_player.py')
package = importlib.util.module_from_spec(spec); spec.loader.exec_module(package)
work = Path(tempfile.mkdtemp(prefix='player-package-', dir=ROOT/'scratch'))
fixture = work/'source'; fixture.mkdir()
(fixture/'config').mkdir()
(fixture/'CMakeLists.txt').write_text('project(pt5 VERSION 0.1.0)\n')
elf = work/'private.elf'
header = bytearray(20); header[:6] = b'\x7fELF\x02\x02'; header[18:20] = b'\0\x15'; elf.write_bytes(header)
(fixture/'config/pt5-player.json').write_text(json.dumps({'elf_bytes':20,'elf_sha256':hashlib.sha256(header).hexdigest()}))
files = {}
for name, content in {'gt5.exe': b'MZsynthetic engine', 'tourline5.exe': b'MZsynthetic launcher',
                      'START-HERE.txt': b'synthetic instructions'}.items():
    source=fixture/name; source.write_bytes(content); files[name]=source
generated={'licenses/NOTICE.txt':'synthetic notice'}
files['recomplauncher.json'] = package.recomplauncher_manifest()
# The production collector must refuse a manifest that bypasses native setup.
(fixture/'launcher').mkdir()
manifest = fixture/'launcher/recomplauncher.json'
for program, valid in [('tourline5.exe', True), ('gt5.exe', False)]:
    manifest.write_text(json.dumps({'format': 1, 'start': {'program': [program]}}))
    with patch.object(package, 'ROOT', fixture):
        try:
            package.recomplauncher_manifest()
            assert valid
        except ValueError:
            assert not valid


def run(*args, fails=False):
    argv=['package', '--build-dir', str(fixture), '--elf', str(elf), '--runtime',str(fixture),
          '--vc-runtime-dir',str(fixture), *map(str,args)]
    with patch.object(package,'ROOT',fixture), patch.object(package,'collect',return_value=(files,dict(generated))), patch.object(sys,'argv',argv):
        try:
            package.main()
        except ValueError:
            assert fails; return
        assert not fails, 'Expected refusal'


stage=work/'player'; archive=work/'player.zip'
run('--stage-dir',stage,'--output',archive)
expected={*files,*generated,'release.ini','pt5-release.ini'}
assert {p.relative_to(stage).as_posix() for p in stage.rglob('*') if p.is_file()} == expected
with zipfile.ZipFile(archive) as z:
    assert set(z.namelist()) == expected
    assert z.read('gt5.exe') == files['gt5.exe'].read_bytes()
    assert json.loads(z.read('recomplauncher.json'))['start']['program'] == ['tourline5.exe']
    assert 'engine_sha256='+package.sha(files['gt5.exe']) in z.read('pt5-release.ini').decode()
    assert all(not z.read(name).startswith(b'\x7fELF') for name in z.namelist())
run('--stage-dir',stage,fails=True)
run('--output',archive,fails=True)
run('--stage-dir',fixture,fails=True)
elf.write_bytes(header+b'wrong')
run('--stage-dir',work/'wrong-identity',fails=True)
elf.write_bytes(header)
original=package.shutil.copy2


def changed(source,destination):
    original(source,destination)
    if Path(source).name=='gt5.exe': Path(source).write_bytes(b'MZchanged during copy')


with patch.object(package.shutil,'copy2',changed):
    run('--stage-dir',work/'changed',fails=True)
# Exercise source-build staging without configuring or rebuilding the shared build/.
cmake_stage = work/'cmake-stage'; cmake_stage.mkdir()
subprocess.run(['cmake', '-DPT5_ENGINE='+str(files['gt5.exe']), '-DPT5_DEST='+str(cmake_stage),
                '-DPT5_SOURCE='+str(ROOT), '-P', str(ROOT/'scripts/stage_player.cmake')], check=True)
assert (cmake_stage/'recomplauncher.json').read_bytes() == files['recomplauncher.json'].read_bytes()
print('PASS: stage/ZIP allowlist, launcher/engine manifests, no ELF, existing destinations, wrong identity and changing inputs')
print('Evidence:',work)
