"""Fetch the pinned DirectX Shader Compiler used for GT5 guest shaders.

    python scripts/get_dxc.py [--zip <already downloaded release zip>] [--dest third_party/dxc]

Downloads the pinned official release from github.com/microsoft/DirectXShaderCompiler, checks its SHA-256, and
extracts only bin/x64/dxcompiler.dll, bin/x64/dxil.dll and the three license files into third_party/dxc/
(gitignored). The build copies the DLLs beside gt5.exe, and the licenses under licenses/dxc.
Without the pair the engine falls back to FXC.
"""
import argparse
import hashlib
from pathlib import Path
import sys
import tempfile
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
VERSION = 'v1.8.2502'
URL = 'https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.8.2502/dxc_2025_02_20.zip'
ZIP_SHA256 = '70b1913a1bfce4a3e1a5311d16246f4ecdf3a3e613abec8aa529e57668426f85'
FILES = {
    'bin/x64/dxcompiler.dll': ('dxcompiler.dll', 'f79943b02f73b621421b58a569fd6ee120d0023fb6bfa23844ecad6b00d6a295'),
    'bin/x64/dxil.dll': ('dxil.dll', '62736e99aa534a942873f92b881a71b2cb6ad62b8d7481ea7a8ad8e9888a8056'),
    'LICENSE-LLVM.txt': ('LICENSE-LLVM.txt', '729615317e28dd03907e46f0fc3b5e88f7853cee61d1a1471d2749335516b46f'),
    'LICENSE-MIT.txt': ('LICENSE-MIT.txt', '903df5512f7d02609fed0c780a9b704f5a3eeb6e4d84ebe42a29845c81899a3c'),
    'LICENSE-MS.txt': ('LICENSE-MS.txt', '734f72f239fe7b07b4c7203f294c1a7ce27095687278bab7e56d630d7c672963'),
}
MAX_ZIP = 64 * 1024 * 1024


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def installed(dest):
    """True when every pinned file is present with its pinned digest."""
    for name, digest in FILES.values():
        p = dest / name
        if not p.is_file() or (digest and sha256(p.read_bytes()) != digest):
            return False
    return True


def extract(zip_path, dest):
    if Path(zip_path).stat().st_size > MAX_ZIP:
        raise ValueError('DXC archive exceeds size limit')
    data = Path(zip_path).read_bytes()
    if len(data) > MAX_ZIP or sha256(data) != ZIP_SHA256:
        raise ValueError('DXC release zip does not match the pinned ' + VERSION + ' digest')
    verified = {}
    with zipfile.ZipFile(zip_path) as z:
        for member, (name, digest) in FILES.items():
            content = z.read(member)
            if digest and sha256(content) != digest:
                raise ValueError('Unexpected ' + member + ' in the DXC release')
            verified[name] = content
    dest.mkdir(parents=True, exist_ok=True)
    for name, content in verified.items():
        tmp = dest / (name + '.tmp')
        tmp.write_bytes(content)
        tmp.replace(dest / name)
    (dest / 'VERSION.txt').write_text(f'DirectX Shader Compiler {VERSION}\n{URL}\nzip sha256 {ZIP_SHA256}\n',
                                      encoding='ascii')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--zip', type=Path, help='use this downloaded release zip instead of downloading')
    ap.add_argument('--dest', type=Path, default=ROOT / 'third_party/dxc')
    a = ap.parse_args()
    if installed(a.dest) and not a.zip:
        print('DXC', VERSION, 'already in', a.dest)
        return
    if a.zip:
        extract(a.zip, a.dest)
    else:
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / 'dxc.zip'
            print('Downloading', URL, flush=True)
            with urllib.request.urlopen(URL, timeout=120) as r, path.open('wb') as f:
                total = 0
                while chunk := r.read(1024 * 1024):
                    total += len(chunk)
                    if total > MAX_ZIP:
                        raise ValueError('DXC archive exceeds size limit')
                    f.write(chunk)
            extract(path, a.dest)
    if not installed(a.dest):
        sys.exit('DXC files did not verify')
    print('DXC', VERSION, 'is in', a.dest)


if __name__ == '__main__':
    main()
