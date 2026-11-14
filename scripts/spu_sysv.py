"""Make an optional private System V copy of fully post-patched SPU sources.

Run after native, regpromo and region patches. Runtime function pointers and
public names remain Windows wrappers. Only direct calls use System V bodies;
that includes the generated region/promotion helpers, preserving musttail.
Never rewrites the input. --output must be a different, empty directory.
"""
import argparse
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parent.parent
DEF = re.compile(r'^(static )?void (\w+)\(spu_context\* ctx\) \{', re.M)
ABI = '''
#if defined(_WIN64) && defined(__clang__) && defined(_M_X64) && !defined(SPU_NO_SYSV)
#define GT5P_CODE_ABI __attribute__((sysv_abi))
#else
#define GT5P_CODE_ABI
#endif
'''


def transform(source):
    if 'GT5P_CODE_ABI' in source:
        raise ValueError('source already converted')
    definitions = list(DEF.finditer(source))
    names = {m[2] for m in definitions}
    if not names:
        raise ValueError('no lifted definitions')
    # Only calls are renamed; bare references (tables, trampoline pointers,
    # native check callbacks) continue to point to their ABI-safe wrappers.
    calls = re.compile(r'\b('+'|'.join(sorted(names,key=len,reverse=True))+r')\(ctx\)')
    text = calls.sub(lambda m: m[1]+'_sv(ctx)', source)
    # Bodies are TU-local so the FMA build's macro aliases cannot cause duplicate
    # global _sv symbols when portable and native copies link into one game.
    text = DEF.sub(lambda m: 'static GT5P_CODE_ABI __attribute__((noinline)) void '+m[2]+'_sv(spu_context* ctx) {',text)
    prototypes = '\n'.join('static GT5P_CODE_ABI __attribute__((noinline)) void '+m[2]+'_sv(spu_context* ctx);' for m in definitions)
    wrappers = '\n'.join((m[1] or '')+'void '+m[2]+'(spu_context* ctx) { '+m[2]+'_sv(ctx); }' for m in definitions)
    # Original static helper references also need declarations before use.
    original_static = '\n'.join('static void '+m[2]+'(spu_context* ctx);' for m in definitions if m[1])
    first = text.index('\n',text.index('#include "../../runtime/spu/spu_helpers.h"'))
    text = text[:first]+ABI+prototypes+'\n'+original_static+'\n'+text[first:]
    return text+'\n'+wrappers+'\n#undef GT5P_CODE_ABI\n'


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--input',type=Path,default=ROOT/'generated/spu_lifted')
    ap.add_argument('--output',type=Path,required=True)
    a = ap.parse_args()
    if a.output.resolve() == a.input.resolve() or (a.output.exists() and any(a.output.iterdir())):
        raise SystemExit('Output must be a separate empty directory')
    count = 0
    for path in sorted(a.input.glob('*/spu_recomp.c')):
        dest = a.output/path.parent.name
        dest.mkdir(parents=True,exist_ok=True)
        (dest/path.name).write_text(transform(path.read_text()),newline='\n')
        (dest/'spu_recomp.h').write_bytes(path.with_suffix('.h').read_bytes())
        count += 1
    print(f'System V copies: {count} images -> {a.output}')


if __name__ == '__main__':
    main()
