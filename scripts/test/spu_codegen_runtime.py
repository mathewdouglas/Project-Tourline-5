"""Offline candidate fingerprint and shuffle checks; all children use idle priority."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
VC = r'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat'
CLANG = r'C:\Program Files\LLVM\bin\clang-cl.exe'
IDLE = subprocess.IDLE_PRIORITY_CLASS


def run(cmd, **kwargs):
    return subprocess.run(cmd, check=True, creationflags=IDLE, **kwargs)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--runtime', type=Path, default=ROOT/'scratch/spu-codegen/candidate')
    ap.add_argument('--bench',action='store_true',help='include synthetic thread-cycle benchmark')
    ap.add_argument('--helpers-only',action='store_true',help='check adopted helpers against a production runtime')
    a = ap.parse_args()
    rt = a.runtime.resolve()
    out = Path(tempfile.mkdtemp(prefix='checks-', dir=ROOT/'scratch/spu-codegen'))
    checks = [
        ('fingerprint', ROOT/'scripts/test/spu_codegen_runtime.c', ''),
        ('shuffle-old', rt/'runtime/spu/tests/test_spu_shufb.c', ''),
        ('shuffle-or', rt/'runtime/spu/tests/test_spu_shufb.c', '/DSPU_SHUFB_OR'),
        ('helpers', ROOT/'scripts/test/spu_codegen_helpers.c', ''),
        ('ls-base', ROOT/'scripts/test/spu_codegen_ls.c', ''),
    ]
    if a.helpers_only:
        checks=[item for item in checks if item[0]=='helpers']
    if a.bench:
        checks.append(('bench',ROOT/'scripts/test/spu_codegen_bench.c',''))
    for name, source, flags in checks:
        batch = out/f'{name}.cmd'
        batch.write_text(f'@echo off\nset "PATH=C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer;%PATH%"\ncall "{VC}" >nul\n'
                         f'"{CLANG}" /nologo /O2 /MD /D_CRT_SECURE_NO_WARNINGS -msse4.1 {flags} '
                         f'/I"{rt}/runtime/spu" /Fo"{out}/{name}.obj" '
                         f'/Fe"{out}/{name}.exe" "{source}"\n')
        run(['cmd','/c',str(batch)])
        if name == 'fingerprint':
            for slots in (1,8,16,64,128,256,0,999):
                run([str(out/f'{name}.exe'),str(slots)],timeout=60)
            run([str(out/f'{name}.exe'),'64','verify'],timeout=60)
        else:
            run([str(out/f'{name}.exe')],timeout=60)
    print(f'Evidence: {out}')


if __name__ == '__main__':
    main()
