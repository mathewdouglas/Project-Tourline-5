"""Pinned DXC archive integrity and required license preservation, with synthetic bytes."""
import hashlib
from pathlib import Path
import sys
import tempfile
from unittest.mock import patch
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import get_dxc

with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary)
    archive = root / 'release.zip'
    members = {'bin/x64/dxcompiler.dll': b'synthetic compiler',
               'bin/x64/dxil.dll': b'synthetic signer',
               'LICENSE-LLVM.txt': b'synthetic LLVM notice',
               'LICENSE-MIT.txt': b'synthetic MIT notice',
               'LICENSE-MS.txt': b'synthetic Microsoft notice'}
    with zipfile.ZipFile(archive, 'w') as output:
        for name, data in members.items():
            output.writestr(name, data)
    expected = {name: (Path(name).name, hashlib.sha256(data).hexdigest())
                for name, data in members.items()}
    with patch.object(get_dxc, 'FILES', expected), patch.object(
            get_dxc, 'ZIP_SHA256', hashlib.sha256(archive.read_bytes()).hexdigest()):
        dest = root / 'verified'
        get_dxc.extract(archive, dest)
        assert get_dxc.installed(dest)
        (dest / 'LICENSE-MS.txt').write_bytes(b'altered notice')
        assert not get_dxc.installed(dest)
        (dest / 'LICENSE-MS.txt').write_bytes(members['LICENSE-MS.txt'])
        (dest / 'dxil.dll').unlink()
        assert not get_dxc.installed(dest)
        archive.write_bytes(archive.read_bytes() + b'corrupt')
        try:
            get_dxc.extract(archive, root / 'rejected')
        except ValueError:
            assert not (root / 'rejected').exists()
        else:
            raise AssertionError('Corrupt release archive accepted')
print('DXC archive, signer and notice integrity checks passed')
