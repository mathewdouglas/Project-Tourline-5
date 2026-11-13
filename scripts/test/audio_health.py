"""Compile production audio with fake COM; exercise invalid PCM and startup errors.

No game, real audio stream, runtime working-tree or build output is touched.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', type=Path, help='test an already prepared runtime copy')
    args = parser.parse_args()
    folder = Path(tempfile.mkdtemp(prefix='audio-health-', dir=ROOT / 'scratch'))
    manifest = json.loads((ROOT / 'config/ps3recomp.json').read_text())
    if args.runtime:
        stage = args.runtime.resolve()
    else:
        runtime = Path(os.environ.get('PS3RECOMP_DIR', ROOT.parent / manifest['directory']))
        stage = folder / 'runtime'
        stage.mkdir()
        env = dict(os.environ, GIT_INDEX_FILE=str(folder / 'private.index'))
        git = ['git', '-C', str(runtime)]
        subprocess.run(git + ['read-tree', manifest['commit']], env=env, check=True)
        subprocess.run(git + ['-c', 'core.autocrlf=true', 'checkout-index', '--all',
                              f'--prefix={stage.as_posix()}/'], env=env, check=True)
        subprocess.run(['git', 'apply', '--unsafe-paths', '--whitespace=nowarn',
                        f'--directory={stage.as_posix()}', str(ROOT / manifest['patch'])], check=True)
    batch = folder / 'build.cmd'
    batch.write_text(
        '@echo off\nset "PATH=C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer;%PATH%"\n'
        'call "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\n'
        'if errorlevel 1 exit /b 1\n'
        '"C:\\Program Files\\LLVM\\bin\\clang-cl.exe" /nologo /O2 /Gy /MD /std:c17 '
        f'/D_CRT_SECURE_NO_WARNINGS /I"{stage}" /I"{stage}/include" '
        f'/Fo"{folder}/test.obj" /Fe"{folder}/test.exe" "{ROOT}/scripts/test/audio_health.c" '
        '/link /OPT:REF ole32.lib user32.lib\n', encoding='ascii')
    subprocess.run(['cmd', '/c', str(batch)], check=True)
    expected = {'enum': ('Create device enumerator', '80070005', 2),
                'endpoint': ('Get default output', '80070490', 2),
                'com': ('COM initialization', '80070005', 1)}
    for case in ('stats', 'mix', 'mix-off', 'mix-healthy', 'mix-inf', 'enum', 'endpoint', 'com'):
        result = subprocess.run([str(folder / 'test.exe'), case], check=True,
                                capture_output=True, text=True, timeout=10)
        if case in expected:
            stage_name, hr, count = expected[case]
            assert result.stderr.count(stage_name + ' failed:') == count, result.stderr
            assert hr in result.stderr, result.stderr
        print(case + ': ' + result.stdout.strip())
    print('PASS: startup HRESULT logs and retry rate limits; no real COM output/device used')


if __name__ == '__main__':
    main()
