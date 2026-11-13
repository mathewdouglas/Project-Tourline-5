"""Compiler launcher for profile-guided (PGO) builds; scripts/pgo/pgo.py sets it
as CMAKE_C_COMPILER_LAUNCHER and CMAKE_CXX_COMPILER_LAUNCHER of build_pgo_gen/
and build_pgo_use/. The normal build/ never uses it.

    pgocc.py gen <dir>        <compiler> <args...>   IR instrumentation
    pgocc.py use <profdata>   <compiler> <args...>   optimise from a profile

The profile flags go right after the compiler, so they are part of every
command ninja hashes: a new profile path recompiles everything, and no CMake
file carries them. CMake's own try_compile checks (CMakeScratch, CMakeTmp) run
unchanged.

gen flags:
  -fprofile-generate=<dir>              IR-level counters (front-end
                                        -fprofile-instr-generate gives no hot/cold
                                        layout in the optimised build)
  -mllvm -runtime-counter-relocation    needed on Windows for continuous mode
                                        (LLVM_PROFILE_FILE=...%c): counters live
                                        in the mapped file, so a killed or
                                        crashed game keeps its counts
  -mllvm -disable-vp=true               continuous mode keeps no value profiles
use flags:
  -fprofile-use=<profdata> -Wno-profile-instr-unprofiled
  -mllvm -pgo-warn-missing-function     names each function the profile has
                                        no record of ("no profile data
                                        available"), so a profile that fits
                                        nothing cannot pass unnoticed
both:
  -mllvm -static-func-full-module-prefix=false   a static function's profile
                                        name uses its file's base name, so a
                                        profile fits a checkout in another folder

The approach follows Soulbrandt's tools/pgo (MIT; JonathanDC64/Soulbrandt,
docs/58); this file is written for this build.
"""
import os
import subprocess
import sys


def main() -> int:
    if len(sys.argv) < 4:
        sys.stderr.write('usage: pgocc.py gen|use <dir or profdata> <compiler> <args...>\n')
        return 2
    mode, arg, cmd = sys.argv[1], sys.argv[2], sys.argv[3:]
    if any('CMakeScratch' in a or 'CMakeTmp' in a for a in cmd):
        return subprocess.call(cmd)
    path = os.path.abspath(arg).replace('\\', '/')
    keep_names = ['-mllvm', '-static-func-full-module-prefix=false']
    if mode == 'gen':
        extra = ['-fprofile-generate=' + path, '-mllvm', '-runtime-counter-relocation',
                 '-mllvm', '-disable-vp=true'] + keep_names
    elif mode == 'use':
        if not os.path.isfile(path):
            sys.stderr.write(f'pgocc.py: no profile at {path}\n')
            return 2
        extra = (['-fprofile-use=' + path] + keep_names +
                 ['-Wno-profile-instr-unprofiled', '-mllvm', '-pgo-warn-missing-function'])
    else:
        sys.stderr.write(f'pgocc.py: unknown mode {mode}\n')
        return 2
    return subprocess.call(cmd[:1] + extra + cmd[1:])


if __name__ == '__main__':
    sys.exit(main())
