"""Link a private executable using existing PPU objects and private SPU objects.

No normal object, library, executable, runtime or cache is modified. This is
for local acceptance/A-B only; release builds must use the clean-build pipeline.
"""
import argparse
import ctypes
import hashlib
from pathlib import Path
import re
import shutil
import subprocess

ROOT=Path(__file__).resolve().parents[2]
LLVM=Path(r'C:\Program Files\LLVM\bin')


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--variant',default='lazy-sysv')
    ap.add_argument('--cache-only',action='store_true')
    ap.add_argument('--objects',default='objects')
    ap.add_argument('--name',default='')
    ap.add_argument('--image',action='append',help='replace only these images, retaining other normal objects')
    a=ap.parse_args()
    ctypes.windll.kernel32.SetPriorityClass(ctypes.windll.kernel32.GetCurrentProcess(),0x40)
    base=ROOT/'scratch/spu-codegen'
    normal=ROOT/'build'
    label=a.name or ('cache-only' if a.cache_only else a.variant)
    if Path(label).name != label or Path(a.objects).name != a.objects:
        raise SystemExit('name and objects must be single directory names')
    dest=base/label/'player'
    dest.mkdir(parents=True,exist_ok=True)
    ninja=(normal/'build.ninja').read_text()
    start=ninja.index('build gt5.exe: ')
    block=ninja[start:ninja.index('\n\n',start)]
    object_line=block.splitlines()[0].split(' ',3)[3].split(' |')[0]
    objects=object_line.split()
    result=[]
    for obj in objects:
        match=re.search(r'generated[\\/]spu_lifted[\\/]([^\\/]+)[\\/]spu_recomp.c.obj$',obj)
        native=re.search(r'audio_native_(\w+)\.c.obj$',obj)
        image=match[1] if match else native[1] if native else None
        if not a.cache_only and image and (not a.image or image in a.image):
            name=match[1] if match else native[1]+'-fma'
            path=base/a.variant/a.objects/f'{name}.obj'
        else:
            path=normal/obj
        if not path.is_file():
            raise SystemExit(f'Missing object: {path}')
        result.append(str(path.resolve()))
    # Recompile only workload dispatch for the private cache experiment. The
    # rest of the runtime library is an immutable copy of the normal library.
    # Full source builds can instead use CMake with the private runtime path.
    lib=dest/'ps3recomp_runtime.lib'
    shutil.copy2(normal/'ps3recomp/ps3recomp_runtime.lib',lib)
    vc=r'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat'
    command=(f'"{LLVM}/clang-cl.exe" /nologo /c /O2 /MD /W0 /D_CRT_SECURE_NO_WARNINGS '
             f'/I"{base}/candidate/include" /I"{base}/candidate/runtime" '
             f'/Fo"{dest}/spu_workload.c.obj" "{base}/candidate/runtime/spu/spu_workload.c"')
    batch=dest/'link.cmd'
    libs=re.search(r'^  LINK_LIBRARIES = (.*)$',block,re.M)[1].split()
    libs=[str(lib) if 'ps3recomp_runtime.lib' in item else item for item in libs]
    rsp=dest/'link.rsp'
    rsp.write_text('\n'.join('"'+v+'"' for v in result+libs))
    batch.write_text('@echo off\nset "PATH=C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer;%PATH%"\n'+f'call "{vc}" >nul\n'+
        command+'\nif errorlevel 1 exit /b 1\n'+
        f'"{LLVM}/llvm-ar.exe" d "{lib}" spu_workload.c.obj\nif errorlevel 1 exit /b 1\n'+
        f'"{LLVM}/llvm-ar.exe" r "{lib}" "{dest}/spu_workload.c.obj"\nif errorlevel 1 exit /b 1\n'+
        f'"{LLVM}/lld-link.exe" /nologo /incremental:no /subsystem:console /out:"{dest}/gt5.exe" /pdb:"{dest}/gt5.pdb" /map:"{dest}/gt5.map" @"{rsp}"\n')
    r=subprocess.run(['cmd','/c',str(batch)],capture_output=True,text=True,cwd=dest)
    (dest/'link.log').write_text(r.stdout+r.stderr)
    if r.returncode:
        raise SystemExit(r.stdout[-3000:]+r.stderr[-3000:])
    for dll in normal.glob('*.dll'):
        shutil.copy2(dll,dest/dll.name)
    exe=dest/'gt5.exe'
    print(exe)
    print('sha256',hashlib.sha256(exe.read_bytes()).hexdigest())


if __name__=='__main__':
    main()
