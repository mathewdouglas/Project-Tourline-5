"""Verify the production bundle reader/writer against Python-produced fixtures."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

root=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(root/'scripts'))
import dxbc_bundle
runtime=Path(os.environ.get('PS3RECOMP_DIR',root.parent/'ps3recomp-tourline5'))
work=Path(tempfile.mkdtemp(prefix='dxbc-bundle-native-',dir=root/'scratch'))
source=(runtime/'libs/video/rsx_d3d12_backend.c').read_text(encoding='utf-8')
first=source.index('/* RSX_DXBC_BUNDLE (on;')
block=source[first:source.index('/* RSX_SHADER_PACK (on;',first)]
(work/'bundle.inc').write_text(block,encoding='utf-8')
(work/'test.c').write_text(r'''
#define CINTERFACE
#define COBJMACROS
#include <windows.h>
#include <d3dcompiler.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint64_t u64;typedef uint32_t u32;typedef uint8_t u8;
static void ps3_name_thread(const wchar_t* s) { (void)s; }
#include "bundle.inc"
int main(int argc,char** argv) {
    assert(argc==3);_putenv_s("RSX_SHADER_CACHE",argv[1]);
    _putenv_s("RSX_DXBC_BUNDLE_IMPORT","0");
    assert(dxb_check("123456789",9)==0xcbf43926u);
    ID3DBlob* b=NULL;
    if(!strcmp(argv[2],"read")) {
        assert(dxb_get(1,&b));assert(b->lpVtbl->GetBufferSize(b)==12);
        assert(!memcmp(b->lpVtbl->GetBufferPointer(b),"shader-one!!",12));
        b->lpVtbl->Release(b);b=NULL;
        assert(!dxb_get(999,&b));
        dxb_put_bytes(2,"shader-two!!",12);
        assert(dxb_get(2,&b));assert(!memcmp(b->lpVtbl->GetBufferPointer(b),"shader-two!!",12));
        b->lpVtbl->Release(b);
    } else if(!strcmp(argv[2],"corrupt")) {
        assert(!dxb_get(1,&b));assert(!dxb_has(1));
        dxb_put_bytes(1,"shader-one!!",12);
        assert(dxb_get(1,&b));b->lpVtbl->Release(b);
    } else {
        assert(!dxb_get(1,&b));assert(s_dxb_state==-1);
    }
    dxb_shutdown();puts("PASS: native bundle fixture");return 0;
}
''',encoding='ascii')
(work/'build.cmd').write_text(f'''@echo off
set "PATH=C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer;%PATH%"
call "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
"C:\\Program Files\\LLVM\\bin\\clang-cl.exe" /nologo /O2 /MD /D_CRT_SECURE_NO_WARNINGS /Fo"{work}/test.obj" /Fe"{work}/test.exe" "{work}/test.c" /link d3dcompiler.lib
''',encoding='ascii')
subprocess.run(['cmd','/c',str(work/'build.cmd')],check=True)
cache=work/'cache';cache.mkdir()
(cache/'0000000000000001.dxbc').write_bytes(b'shader-one!!')
dxbc_bundle.build(cache)
env=dict(os.environ,RSX_DXBC_BUNDLE='1')
subprocess.run([str(work/'test.exe'),str(cache),'read'],env=env,check=True)
assert dxbc_bundle.check(cache)==(2,0)
data=bytearray((cache/'dxbc-bundle-v1.dat').read_bytes());data[16]^=1
(cache/'dxbc-bundle-v1.dat').write_bytes(data)
subprocess.run([str(work/'test.exe'),str(cache),'corrupt'],env=env,check=True)
assert dxbc_bundle.check(cache)==(2,1) # rejected old record stays append-only
subprocess.run([str(work/'test.exe'),str(cache),'off'],env=dict(env,RSX_DXBC_BUNDLE='0'),check=True)
subprocess.run([str(work/'test.exe'),str(cache),'off'],env={**{k:v for k,v in env.items() if k!='RSX_DXBC_BUNDLE'},'GT5P_SHADER_PRECOMPILE_ONLY':'1'},check=True)
print('Evidence:',work)
