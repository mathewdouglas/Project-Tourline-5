r"""Exercise launcher/storage_link.h through storage_link_probe.exe in private temporary folders.

Build: cl /nologo /std:c++17 /EHsc /DUNICODE /D_UNICODE scripts\test\storage_link_probe.cpp
then: python scripts/test/storage_link.py --probe storage_link_probe.exe
Launcher integration is covered by scripts/test/player_setup.py with private profiles and a fixture engine.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--probe', type=Path, required=True)
    args = ap.parse_args()
    probe = args.probe.resolve()
    with tempfile.TemporaryDirectory(prefix='PT5 storage link ') as temp:
        root = Path(temp)
        home, first, second = root/'Home', root/'Storage one', root/'Storage two'
        for p in (home, first, second):
            p.mkdir()
        (first/'gt5p_settings.toml').write_text('[video]\nresolution = "720p"\n')
        (second/'gt5p_settings.toml').write_text('[video]\nresolution = "1080p"\n')
        link = home/'selected-storage'

        def run(target, expect=0):
            r = subprocess.run([str(probe), str(home), str(target) if target else '-'], capture_output=True, text=True)
            assert r.returncode == expect, (target, r.returncode, r.stdout)
            return r.stdout.strip()

        def points_to(path):
            assert link.is_junction(), 'not a junction'
            assert os.path.samefile(link, path), (os.readlink(link), path)

        run(None)
        assert not link.exists(), 'default storage needs no link'
        run(first)
        points_to(first)
        assert 'resolution = "720p"' in (link/'gt5p_settings.toml').read_text()
        (link/'written-through-link.txt').write_text('x')
        assert (first/'written-through-link.txt').exists()
        before = os.stat(link).st_ctime_ns
        run(first)
        points_to(first)
        assert os.stat(link).st_ctime_ns == before, 'an unchanged link is recreated'
        disconnected = root/'Disconnected'
        first.rename(disconnected)
        assert not link.exists(), 'disconnected target should dangle'
        run(str(second) + os.sep)
        points_to(second)
        disconnected.rename(first)
        assert 'resolution = "1080p"' in (link/'gt5p_settings.toml').read_text()
        run(None)
        assert not link.exists()
        assert (first/'gt5p_settings.toml').exists() and (second/'gt5p_settings.toml').exists(), 'storage was removed'
        drive = Path(Path(temp).anchor)
        run(drive)
        points_to(drive)
        run(None)
        print('create, reuse, retarget, drive root and removal: passed')

        link.mkdir()
        (link/'real.txt').write_text('keep')
        assert 'not a junction' in run(first, 1)
        assert (link/'real.txt').read_text() == 'keep'
        run(None, 1)
        assert (link/'real.txt').exists()
        (link/'real.txt').unlink(); link.rmdir()
        assert 'network' in run(2 * os.sep + os.path.join('server', 'share', 'PT5'), 1)
        assert not link.exists()
        print('real folder kept, network target refused: passed')


if __name__ == '__main__':
    main()
