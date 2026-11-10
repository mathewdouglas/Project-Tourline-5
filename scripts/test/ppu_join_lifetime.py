"""Deterministically hold a PPU host epilogue after guest exit while joining.

Extracts only repository-pinned runtime functions; no game input or GPU needed.
Run sequentially under isolated_validation.ps1. --runtime selects an isolated
runtime with the compatibility patch; --expect-legacy proves the old failure.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--runtime',type=Path,required=True)
parser.add_argument('--expect-legacy',action='store_true')
parser.add_argument('--source',type=Path,help='Optional baseline source; headers still come from --runtime')
args=parser.parse_args()
root=Path(__file__).resolve().parents[2]
runtime=args.runtime.resolve()
folder=Path(tempfile.mkdtemp(prefix='ppu-join-',dir=root/'scratch'))
source=(args.source or runtime/'runtime/syscalls/sys_ppu_thread.c').read_text(encoding='utf-8')
def function(name):
    start=source.index(name)
    start=source.rfind('\n',0,start)+1
    brace=source.index('{',start); depth=1; end=brace+1
    # These functions contain no braces in strings or comments.
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]
parts=[function('static void table_lock('),function('static void table_unlock('),
       function('static ppu_thread_info* find_thread('),
       function('int64_t sys_ppu_thread_exit('),function('int64_t sys_ppu_thread_join(')]
# Choose Windows signature rather than retaining the alternative POSIX signature.
a=source.index('static DWORD WINAPI ppu_host_thread_proc(')
b=source.index('\n/* ---------------------------------------------------------------------------',a)
host=source[a:b].replace('#else\nstatic void* ppu_host_thread_proc(void* param)\n#endif\n','')
# The legacy GT5 runtime predates thread-profiler exit hooks. Hold the same
# boundary explicitly in this extracted fixture; no production source changes.
if 'ps3_tprof_thread_exit();' not in host:
    host=host.replace('    /* Mark as finished */', '    ps3_tprof_thread_exit();\n    /* Mark as finished */')
(folder/'runtime_join.inc').write_text('\n\n'.join(parts)+'\n'+host,encoding='utf-8')
fixture=(root/'scripts/test/ppu_join_lifetime.c').read_text(encoding='utf-8')
(folder/'fixture.c').write_text(fixture,encoding='utf-8')
batch=folder/'build.cmd'
batch.write_text('@echo off\nset "PATH=C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer;%PATH%"\ncall "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\n'
 + f'"C:\\Program Files\\LLVM\\bin\\clang-cl.exe" /nologo /O2 /MD /D_CRT_SECURE_NO_WARNINGS /I"{runtime}/runtime/syscalls" /I"{runtime}/include" /I"{folder}" /Fo"{folder}/" /Fe"{folder}/test.exe" "{folder}/fixture.c"\n')
subprocess.run(['cmd','/c',str(batch)],check=True)
result=subprocess.run([str(folder/'test.exe'), 'legacy' if args.expect_legacy else 'fixed'],capture_output=True,text=True,timeout=30)
(folder/'run.log').write_text(result.stdout+result.stderr,encoding='utf-8')
if result.returncode:
    print(result.stdout+result.stderr)
result.check_returncode()
print(result.stdout.strip())
print('evidence:',folder)
