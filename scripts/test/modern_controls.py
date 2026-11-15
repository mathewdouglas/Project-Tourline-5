"""Native GT5 mapping and settings checks; private CPU-only outputs, no game run."""
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
runtime = Path(os.environ.get('PS3RECOMP_DIR', ROOT.parent / 'ps3recomp-tourline5')).resolve()
work = Path(tempfile.mkdtemp(prefix='modern-controls-', dir=ROOT / 'scratch'))
clang = Path('C:/Program Files/LLVM/bin/clang-cl.exe')
vcvars = Path('C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Auxiliary/Build/vcvars64.bat')
probe = work / 'settings_probe.cpp'
probe.write_text(r'''
#include "gt5p_settings.h"
#include <cstdlib>
#include <cstdio>
#include <fstream>
extern "C" void pad_keyboard_set_vk(unsigned,int) {}
void gt5p_glyphs_keys_changed() {}
#define CHECK(x) do { if(!(x)) {fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);return 1;} }while(0)
int main(int argc,char** argv) {
 CHECK(argc==2); _putenv_s("GT5P_SETTINGS",argv[1]);
 gt5p_settings_load();
 CHECK(gt5p_settings_file().modern_controls);
 CHECK(gt5p_settings_file().shader_warmup==false);
 // The persisted choice must not synthesize an override or a boot request.
 CHECK(!getenv("PAD_MODERN_CONTROLS"));
 CHECK(!gt5p_settings_overridden("PAD_MODERN_CONTROLS"));
 auto s=gt5p_settings_file();s.modern_controls=false;
 CHECK(gt5p_settings_store(s));
 CHECK(!gt5p_settings_file().modern_controls);
 puts("PASS: modern-controls settings parse/store, preload-off preservation, no boot command");
}
''', encoding='utf-8')
flags = f'/nologo /O1 /EHsc /MD /std:c++20 /D_CRT_SECURE_NO_WARNINGS /W0 /I"{ROOT}/src" /I"{ROOT}/external/tomlplusplus" /I"{runtime}/include" /I"{runtime}/runtime/ppu"'
commands = [f'"{clang}" {flags} /Fo"{work}/mapping.obj" /Fe"{work}/mapping.exe" "{ROOT}/scripts/test/modern_controls.cpp"']
for src, out in [(ROOT/'src/gt5p_settings.cpp','settings'), (ROOT/'src/gt5p_settings_ui.cpp','ui'), (ROOT/'src/gt5p_keymap.cpp','keymap'), (probe,'probe')]:
    commands.append(f'"{clang}" {flags} /c /Fo"{work}/{out}.obj" "{src}"')
# Compile each executable's concrete definitions, separately from the fixture's synthetic ones.
targets=json.loads((ROOT/'config/targets.json').read_text())
for target in ('BCUS98114-v104','BCUS98114-v211'):
    defs=' '.join(f'/DGT5P_CONTROLS_{key.upper()}={targets[target]["controls_"+key]}u' for key in ('core','vtable','get','set','load','save'))
    commands.append(f'"{clang}" {flags} {defs} /c /Fo"{work}/{target}.obj" "{ROOT}/src/gt5p_controls.cpp"')
commands.append(f'"{clang}" /nologo "{work}/probe.obj" "{work}/settings.obj" "{work}/keymap.obj" /Fe"{work}/settings.exe" /link user32.lib')
batch='@echo off\nset "PATH=C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer;%PATH%"\n'
batch+=f'call "{vcvars}" >nul\nif errorlevel 1 exit /b 1\n'
for command in commands:
    batch+=command+'\nif errorlevel 1 exit /b 1\n'
(work/'build.cmd').write_text(batch,encoding='ascii')
subprocess.run(['cmd','/c',str(work/'build.cmd')],check=True,cwd=work)
env=os.environ.copy()
for key in ('PAD_MODERN_CONTROLS','GT5P_NATIVE_CONTROLS','PAD_CONTROLS_POLISH','GT5P_NO_SETTINGS'):
    env.pop(key,None)
subprocess.run([str(work/'mapping.exe')],check=True,timeout=15,env=env,cwd=work)
settings=work/'settings.toml'
settings.write_text('[input]\nmodern_controls = true\n[video]\nshader_warmup = false\n',encoding='ascii')
subprocess.run([str(work/'settings.exe'),str(settings)],check=True,timeout=15,env=env,cwd=work)
print('Evidence:',work)
