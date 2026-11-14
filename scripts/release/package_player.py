"""Package a completed BCUS98114-v104 Release build as a player ZIP; never builds or runs anything.

Strict allowlist: the engine, its runtime DLLs, glyph assets, licences and
the setup notes. No game files, saves, caches, settings, logs or lifted
sources are copied. The player supplies the game (packaging/START-HERE.txt).
Only the SHA-256 of the matching decrypted executable enters the ZIP.

    python scripts/release/package_player.py --build-dir build --elf input/EMAIN.ELF \
        --runtime ../ps3recomp-tourline5 --vc-runtime-dir <VC Redist x64 CRT> --output out/pt5.zip

Usually called by scripts/release/clean_build.py.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import zipfile

ROOT = Path(__file__).resolve().parents[2]
TARGET = 'BCUS98114-v104'
ENGINE = 'gt5.exe'
RUNTIME_DLLS = ('SDL2.dll', 'avcodec-61.dll', 'avutil-59.dll', 'swresample-5.dll', 'dxcompiler.dll', 'dxil.dll')
VC_DLLS = ('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')   # what gt5.exe imports
DXC_NOTICES = ('LICENSE-LLVM.txt', 'LICENSE-MIT.txt', 'LICENSE-MS.txt', 'VERSION.txt')
# Leading bytes of files that must never be distributed: ELF (decrypted
# executables, SPU images), SELF/SPRX, PARAM.SFO, PS3 package, PUP.
FORBIDDEN_MAGIC = (b'\x7fELF', b'SCE\x00', b'\x00PSF', b'\x7fPKG', b'SCEUF')

FFMPEG_SOURCE = '''FFmpeg in this package
======================

avcodec-61.dll, avutil-59.dll and swresample-5.dll are FFmpeg 7.1, an
unmodified LGPL shared build from BtbN/FFmpeg-Builds:

  build:   {name}
  release: https://github.com/BtbN/FFmpeg-Builds/releases/tag/{tag}
  archive: https://github.com/BtbN/FFmpeg-Builds/releases/download/{tag}/{name}.zip
  sha256:  {sha}

The archive's name identifies the FFmpeg source revision it was built from
(n7.1.5-12-g1fdbca85aa, https://github.com/FFmpeg/FFmpeg/commit/1fdbca85aa).
The build scripts are at https://github.com/BtbN/FFmpeg-Builds. FFmpeg is
licensed under the GNU LGPL; see FFmpeg-LICENSE.txt. The DLLs are loaded at
run time and can be replaced with any compatible build of the same major
versions. scripts/get_ffmpeg.ps1 in the port's repository fetches them.
'''

MSVC_NOTICE = '''Microsoft Visual C++ runtime
============================

msvcp140.dll, vcruntime140.dll and vcruntime140_1.dll are redistributable
files of the Microsoft Visual C++ 2015-2022 runtime (x64), copied from the
Visual Studio 2022 redistributable folder ({folder}) and distributed
under the Visual Studio redistribution terms. They are Microsoft's and are
not covered by this port's licence. If they are removed, install the
"Microsoft Visual C++ Redistributable for Visual Studio 2015-2022 (x64)".
'''

AT3_NOTICE = '''ATRAC3 / ATRAC3plus decoder
===========================

gt5.exe contains the at3_standalone decoder from the ps3recomp runtime
(third_party/at3_standalone, derived from FFmpeg), licensed under the GNU
LGPL 2.1 (at3_standalone-COPYING.LGPLv2.1.txt). The complete source of this
program, including that library, the runtime pin (config/ps3recomp.json)
and the runtime patch, is public at
https://github.com/mathewdouglas/Project-Tourline-5 at tag v{version}, so
you can modify the library and rebuild the program (README, "Building from
source").
'''


def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def regular(path):
    path = Path(path)
    if not path.is_file() or path.is_symlink():
        raise ValueError(f'Missing file or unexpected link: {path}')
    for parent in path.parents:
        if parent.is_symlink() or (hasattr(parent, 'is_junction') and parent.is_junction()):
            raise ValueError(f'Input reached through a link: {path}')
    return path


def pe_x64(path):
    regular(path)
    with path.open('rb') as f:
        dos = f.read(64)
        if len(dos) != 64 or dos[:2] != b'MZ':
            raise ValueError(f'Expected a Windows binary: {path}')
        f.seek(int.from_bytes(dos[60:64], 'little'))
        if f.read(6) != b'PE\0\0\x64\x86':
            raise ValueError(f'Expected an x64 PE binary: {path}')
    return path


def text(path, limit=1_200_000):
    regular(path)
    data = path.read_bytes()
    if len(data) > limit or b'\0' in data:
        raise ValueError(f'Expected a text notice: {path}')
    return path


def port_version():
    m = re.search(r'project\(\s*\S+\s+VERSION\s+(\d+\.\d+\.\d+)', (ROOT / 'CMakeLists.txt').read_text(encoding='utf-8'))
    if not m:
        raise ValueError('CMakeLists.txt project() has no VERSION')
    return m.group(1)


def ffmpeg_pin():
    s = (ROOT / 'scripts/get_ffmpeg.ps1').read_text(encoding='utf-8')
    pick = lambda key: re.search(r'\$' + key + r'\s*=\s*"([^"]+)"', s).group(1)
    return dict(tag=pick('Tag'), name=pick('Name'), sha=pick('Sha256'))


def recomplauncher_manifest():
    path = text(ROOT / 'launcher/recomplauncher.json')
    document = json.loads(path.read_text(encoding='utf-8'))
    if (not isinstance(document, dict) or document.get('format') != 1 or
            document.get('start', {}).get('program') != ['tourline5.exe']):
        raise ValueError('recomplauncher.json must start tourline5.exe')
    return path


def collect(args, version):
    build = args.build_dir.resolve()
    cache = regular(build / 'CMakeCache.txt').read_text(encoding='utf-8', errors='replace')
    if not re.search(rf'^GT5P_TARGET:STRING={re.escape(TARGET)}$', cache, re.M):
        raise ValueError(f'The build must target {TARGET}')
    if not re.search(r'^CMAKE_BUILD_TYPE:STRING=Release$', cache, re.M):
        raise ValueError('Only a completed Release build can be packaged')
    deps = args.third_party.resolve()
    files = {ENGINE: pe_x64(build / ENGINE),
             'tourline5.exe': pe_x64(build / 'tourline5.exe'),
             'LICENSE.txt': text(ROOT / 'LICENSE'),
             'START-HERE.txt': text(ROOT / 'launcher/START-HERE.txt'),
             'CHANGELOG.md': text(ROOT / 'CHANGELOG.md')}
    files['recomplauncher.json'] = recomplauncher_manifest()
    for name in ('Change storage.cmd', 'Change game files.cmd', 'Open storage.cmd'):
        files[name] = text(ROOT / 'launcher' / name)
    for name in RUNTIME_DLLS:
        files[name] = pe_x64(build / name)
    # The DLLs beside the engine must be the pinned downloads.
    pinned = {'SDL2.dll': deps / 'SDL2.dll', 'dxcompiler.dll': deps / 'dxc/dxcompiler.dll',
              'dxil.dll': deps / 'dxc/dxil.dll'}
    pinned.update({n: deps / 'ffmpeg/bin' / n for n in ('avcodec-61.dll', 'avutil-59.dll', 'swresample-5.dll')})
    for name, source in pinned.items():
        if sha(files[name]) != sha(regular(source)):
            raise ValueError(f'{name} beside the engine differs from {source}')
    import importlib.util
    spec = importlib.util.spec_from_file_location('get_dxc', ROOT / 'scripts/get_dxc.py')
    get_dxc = importlib.util.module_from_spec(spec); spec.loader.exec_module(get_dxc)
    if not get_dxc.installed(deps / 'dxc'):
        raise ValueError('third_party/dxc is not the pinned DXC release (scripts/get_dxc.py)')
    vc = args.vc_runtime_dir.resolve()
    for name in VC_DLLS:
        files[name] = pe_x64(vc / name)
    lic = 'licenses/'
    files[lic + 'SDL2-README-SDL.txt'] = text(deps / 'README-SDL.txt')
    files[lic + 'FFmpeg-LICENSE.txt'] = text(deps / 'ffmpeg/LICENSE.txt')
    for name in DXC_NOTICES:
        files[lic + 'DXC-' + name] = text(deps / 'dxc' / name)
    runtime = args.runtime.resolve()
    files[lic + 'ps3recomp-LICENSE.txt'] = text(runtime / 'LICENSE')
    at3 = runtime / 'third_party/at3_standalone'
    files[lic + 'at3_standalone-COPYING.LGPLv2.1.txt'] = text(at3 / 'COPYING.LGPLv2.1')
    files[lic + 'at3_standalone-README.txt'] = text(at3 / 'README.txt')
    files[lic + 'tomlplusplus-LICENSE.txt'] = text(ROOT / 'external/tomlplusplus/LICENSE')
    generated = {lic + 'FFmpeg-SOURCE.txt': FFMPEG_SOURCE.format(**ffmpeg_pin()),
                 lic + 'MSVC-NOTICE.txt': MSVC_NOTICE.format(folder=vc.name if vc.name else str(vc)),
                 lic + 'at3_standalone-NOTICE.txt': AT3_NOTICE.format(version=version)}
    # Glyph art (Kenney, CC0) and its manifest: explicit extensions only.
    glyphs = ROOT / 'assets/glyphs'
    for p in sorted(glyphs.rglob('*')):
        if p.is_symlink() or (hasattr(p, 'is_junction') and p.is_junction()):
            raise ValueError(f'Asset links are not distributable: {p}')
        if not p.is_file():
            continue
        if p.suffix.lower() == '.png':
            with p.open('rb') as f:
                if f.read(8) != b'\x89PNG\r\n\x1a\n':
                    raise ValueError(f'Invalid PNG asset: {p}')
        elif p.suffix.lower() == '.txt':
            text(p, 256_000)
        else:
            raise ValueError(f'Unreviewed asset: {p}')
        files['assets/glyphs/' + p.relative_to(glyphs).as_posix()] = p
    for needed in ('assets/glyphs/textures.txt', 'assets/glyphs/kenney/License.txt'):
        if needed not in files:
            raise ValueError(f'Missing {needed}')
    return files, generated


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--build-dir', required=True, type=Path)
    ap.add_argument('--elf', required=True, type=Path, help='the matching decrypted executable; only its SHA-256 is stored')
    ap.add_argument('--runtime', required=True, type=Path, help='the ps3recomp checkout the build used')
    ap.add_argument('--third-party', type=Path, default=ROOT / 'third_party')
    ap.add_argument('--vc-runtime-dir', required=True, type=Path)
    ap.add_argument('--output', type=Path, help='write a player ZIP')
    ap.add_argument('--stage-dir', type=Path, help='write an unpacked player folder (must be empty)')
    ap.add_argument('--pgo-profile', type=Path, help='the profile a profile-guided build used; only its SHA-256 is stored')
    args = ap.parse_args()
    version = port_version()
    if not args.output and not args.stage_dir:
        ap.error('Supply --output and/or --stage-dir')
    output = args.output.resolve() if args.output else None
    stage = args.stage_dir.resolve() if args.stage_dir else None
    if output and output.exists():
        raise ValueError('Refusing to overwrite an existing package')
    if stage and stage.exists() and (not stage.is_dir() or any(stage.iterdir())):
        raise ValueError('Choose an empty stage directory; existing player folders are never overwritten')
    elf = regular(args.elf)
    with elf.open('rb') as f:
        header = f.read(20)
    if header[:6] != b'\x7fELF\x02\x02' or header[18:20] != b'\x00\x15':
        raise ValueError('Expected the decrypted big-endian PowerPC64 ELF')
    identity = json.loads((ROOT / 'config/pt5-player.json').read_text())
    if elf.stat().st_size != identity['elf_bytes'] or sha(elf) != identity['elf_sha256']:
        raise ValueError('The supplied ELF does not match the launcher\'s BCUS98114 update 01.04 identity')
    files, generated = collect(args, version)
    release = ('[release]\nschema=1\nproduct=Project Tourline 5\n'
               f'port_version={version}\ntarget={TARGET}\ngame=BCUS98114 01.04\n'
               f'elf_sha256={sha(elf)}\nengine_sha256={sha(files[ENGINE])}\n')
    if args.pgo_profile:
        release += f'pgo_profile_sha256={sha(regular(args.pgo_profile))}\n'
    # volmap=1: the engine serves virtual-install records (launcher/volmap.h).
    generated['pt5-release.ini'] = f'[game]\ntarget={TARGET}\nengine_sha256={sha(files[ENGINE])}\nvolmap=1\n'
    before = {n: (p.stat().st_size, p.stat().st_mtime_ns) for n, p in files.items()}
    if stage:
        # Prevent copying a source directory into itself (including via aliases).
        for path in files.values():
            resolved = path.resolve()
            if stage == resolved or stage in resolved.parents:
                raise ValueError('Stage directory must not contain package inputs')
        for folder in (ROOT / 'assets', ROOT / 'launcher', args.build_dir.resolve() / 'assets', args.build_dir.resolve() / 'licenses'):
            if stage.is_relative_to(folder.resolve()):
                raise ValueError('Stage directory must be separate from input assets and licenses')
        stage.mkdir(parents=True, exist_ok=True)
        for name, content in {**generated, 'release.ini': release}.items():
            destination = stage / name; destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text(content, encoding='utf-8')
        for name, source in files.items():
            destination = stage / name; destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, destination)
            if before[name] != (source.stat().st_size, source.stat().st_mtime_ns):
                raise ValueError(f'Input changed while staging: {source}')
            if sha(source) != sha(destination):
                raise ValueError(f'Staged file differs from its input: {source}')
            with destination.open('rb') as f:
                head = f.read(8)
            if any(head.startswith(m) for m in FORBIDDEN_MAGIC):
                raise ValueError(f'Refusing a game-format file: {name}')
        print(f'Staged {stage}: {len(files) + len(generated) + 1} files, no game data')
    if not output:
        return
    output.parent.mkdir(parents=True, exist_ok=True)
    partial = output.with_suffix(output.suffix + '.partial')
    if partial.exists():
        raise ValueError(f'A previous partial package exists: {partial}')
    try:
        with zipfile.ZipFile(partial, 'x', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            z.writestr('release.ini', release)
            for name, content in generated.items():
                z.writestr(name, content)
            for name, p in files.items():
                z.write(p, name)
                if before[name] != (p.stat().st_size, p.stat().st_mtime_ns):
                    raise ValueError(f'Input changed while packaging: {p}')
        with zipfile.ZipFile(partial) as z:
            for info in z.infolist():
                with z.open(info) as f:
                    head = f.read(8)
                if any(head.startswith(m) for m in FORBIDDEN_MAGIC):
                    raise ValueError(f'Refusing a game-format file: {info.filename}')
            with z.open(ENGINE) as f:
                h = hashlib.sha256()
                for chunk in iter(lambda: f.read(1 << 20), b''):
                    h.update(chunk)
            if h.hexdigest() != sha(files[ENGINE]):
                raise ValueError('Packaged engine differs from the manifest')
            names = z.namelist()
        partial.rename(output)
    except BaseException:
        partial.unlink(missing_ok=True)
        raise
    print(f'Created {output}: {len(names)} files, no game data')
    print(f'SHA-256 {sha(output)}')


if __name__ == '__main__':
    main()
