"""Compile private lifted SPU variants, one idle-priority compiler at a time.

Includes every target audio/FMA alias, coefficient-rounding header and generated
region/native hook. Does not configure or touch build/ or the active runtime.
"""
import argparse
import ctypes
import json
from pathlib import Path
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'scripts'))
import target


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--variant',default='lazy-sysv')
    ap.add_argument('--image',action='append')
    ap.add_argument('--helpers',action='store_true')
    ap.add_argument('--shuffle-or',action='store_true')
    ap.add_argument('--objects',default='objects')
    ap.add_argument('--resume',action='store_true')
    a=ap.parse_args()
    ctypes.windll.kernel32.SetPriorityClass(ctypes.windll.kernel32.GetCurrentProcess(),0x40)
    base=ROOT/'scratch/spu-codegen'
    src=base/a.variant/'generated/spu_lifted'
    if Path(a.objects).name != a.objects:
        raise SystemExit('objects must be a single directory name')
    out=base/a.variant/a.objects;out.mkdir(exist_ok=True)
    math=set(target.TARGET.get('audio_extended_zero_images',[]))
    fma={v[0] for v in target.TARGET.get('audio_native_images',[])+target.TARGET.get('spu_fma_images',[])}
    results=[]
    vc=r'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat'
    for path in sorted(src.glob('*/spu_recomp.c')):
        image=path.parent.name
        if a.image and image not in a.image:
            continue
        for native in (False,True) if image in fma else (False,):
            name=image+('-fma' if native else '')
            unit=path
            if native:
                symbols=sorted(set(__import__('re').findall(r'gt5p_'+image+r'_\w+',path.with_suffix('.h').read_text())))
                unit=out/f'{name}.c'
                wrapper=('#define spu_register_function gt5p_audio_register_function\n'+
                    '\n'.join('#define '+s+' '+s.replace('gt5p_','gt5p_audio_',1) for s in symbols)+
                    f'\n#include "{path.as_posix()}"\n')
                if not unit.exists() or unit.read_text()!=wrapper: unit.write_text(wrapper)
            flags='-mfma' if native else ''
            if a.helpers: flags+=' /DSPU_CODEGEN_HELPERS'
            if a.shuffle_or: flags+=' /DSPU_SHUFB_OR'
            if image in math:
                flags+=f' /FI"{ROOT}/src/gt5p_spu_audio_math.h"'
            batch=out/f'{name}.cmd'
            command='@echo off\nset "PATH=C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer;%PATH%"\n'+f'call "{vc}" >nul\n'+\
              f'"C:/Program Files/LLVM/bin/clang-cl.exe" /nologo /c /O2 /MD /W0 /D_CRT_SECURE_NO_WARNINGS /DSPU_LS_NO_PROBES -msse4.1 {flags} /I"{base}/candidate/runtime/spu" /Fo"{out}/{name}.obj" "{unit}"\n'
            dependencies=[path,path.with_suffix('.h'),unit,*list((base/'candidate/runtime/spu').glob('*.h'))]
            if image in math: dependencies.append(ROOT/'src/gt5p_spu_audio_math.h')
            obj=out/f'{name}.obj'
            if a.resume and obj.exists() and batch.exists() and batch.read_text()==command and obj.stat().st_mtime>=max(p.stat().st_mtime for p in dependencies):
                results.append(name);print(name,'reused',flush=True);continue
            batch.write_text(command)
            r=subprocess.run(['cmd','/c',str(batch)],capture_output=True,text=True)
            (out/f'{name}.log').write_text(r.stdout+r.stderr)
            if r.returncode:
                raise SystemExit(f'{name}: compile failed\n{r.stdout[-2500:]}{r.stderr[-500:]}')
            results.append(name)
            print(name,'compiled',flush=True)
    (out/'compiled.json').write_text(json.dumps(results,indent=2))
    print(f'Compiled {len(results)} private objects')


if __name__=='__main__':
    main()
