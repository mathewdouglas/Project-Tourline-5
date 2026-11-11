"""Production launcher integration using synthetic game files and a fixture engine.

No game process/data, RPCS3 installation, GPU or real user profile is touched.
"""
import ctypes
import hashlib
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
WORK = Path(tempfile.mkdtemp(prefix='player-setup-', dir=ROOT / 'scratch'))
ELF = b'\x7fELF synthetic executable for setup tests only\n'
SHA = hashlib.sha256(ELF).hexdigest()
(WORK / 'pt5_launcher_config.h').write_text(f'#define PT5_ELF_SHA256 L"{SHA}"\n#define PT5_ELF_BYTES {len(ELF)}ull\n')
# Compile the fixture's resources here too; never use or rebuild shared build/ outputs.
resource = (ROOT/'launcher/launcher.rc.in').read_text().replace('@CMAKE_SOURCE_DIR@', ROOT.as_posix())
for key, value in {'PROJECT_VERSION': '0.1.0', 'PROJECT_VERSION_MAJOR': '0',
                   'PROJECT_VERSION_MINOR': '1', 'PROJECT_VERSION_PATCH': '0'}.items():
    resource = resource.replace('@'+key+'@', value)
(WORK/'launcher.rc').write_text(resource)
(WORK / 'engine.cpp').write_text(r'''
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include "pt5_restart.h"
int main(int argc, char** argv) {
    if (argc == 3 && !strcmp(argv[1], "--decrypt")) {
        if (getenv("PT5_TEST_DECRYPT_FAIL")) return 7;
        if (!getenv("PT5_TEST_DECRYPT_NO_OUTPUT")) {
            auto out = std::filesystem::path(argv[2]); out.replace_extension(".elf");
            std::filesystem::copy_file(getenv("PT5_TEST_DECRYPT_SOURCE"), out);
        }
        return 0;
    }
    if (!pt5_restart::managed()) return 2;
    printf("cwd=%s\nelf=%s\n", std::filesystem::current_path().string().c_str(),argc>1?argv[1]:"");
    for (const char* key : {"GT5P_GAME_DIR","GT5P_SETTINGS","GT5P_ROOT","PS3_VFS_ROOT","RSX_SHADER_CACHE","GT5P_LAUNCHER_RESTART"})
        printf("%s=%s\n",key,getenv(key)?getenv(key):"");
    if(const char* p=getenv("PT5_TEST_HOLD")) { std::ofstream(p)<<"ready"; Sleep(2000); }
    if(const char* p=getenv("PT5_TEST_RESTART")) {
        if(!std::filesystem::exists(p)) {std::ofstream(p)<<"restarted";return pt5_restart::exit_code;}
    }
    if(const char* p=getenv("PT5_TEST_COUNT")) {std::ofstream(p,std::ios::app)<<"run\n";}
    return 0;
}
''')
(WORK / 'decryption.cpp').write_text(r'''
#define wWinMain pt5_launcher_entrypoint
#include "@LAUNCHER@"
#include <cassert>
int wmain(int argc, wchar_t** argv) {
    assert(argc==2); fs::path work=argv[1];
    data=work/L"decrypt logs"; fs::create_directory(data); quiet=true; silent_errors=true;
    auto source=work/L"RPCS3 folder/dev_hdd0/game/BCUS98114";
    auto before=hash(source/L"USRDIR/EBOOT.BIN");
    auto temp=work/L"temporary copy & spaces"; fs::create_directory(temp);
    auto elf=decrypt_executable(source,temp,work/L"gt5.exe");
    assert(hash(elf)==PT5_ELF_SHA256);
    assert(hash(temp/L"EBOOT.BIN")==before && hash(source/L"USRDIR/EBOOT.BIN")==before);
    SetEnvironmentVariableW(L"PT5_TEST_DECRYPT_FAIL",L"1");
    temp=work/L"failed decryption";fs::create_directory(temp);bool rejected=false;
    try {decrypt_executable(source,temp,work/L"gt5.exe");}catch(const Failure&){rejected=true;}
    assert(rejected);SetEnvironmentVariableW(L"PT5_TEST_DECRYPT_FAIL",nullptr);
    SetEnvironmentVariableW(L"PT5_TEST_DECRYPT_NO_OUTPUT",L"1");
    temp=work/L"missing output";fs::create_directory(temp);rejected=false;
    try {decrypt_executable(source,temp,work/L"gt5.exe");}catch(const Failure&){rejected=true;}
    assert(rejected && hash(source/L"USRDIR/EBOOT.BIN")==before);
    CopyState cancelled;
    assert(copy_dialog(nullptr,TDN_BUTTON_CLICKED,IDCANCEL,0,(LONG_PTR)&cancelled)==S_FALSE);
    assert(cancelled.cancel);
    LARGE_INTEGER zero{};
    assert(copy_progress(zero,zero,zero,zero,0,0,nullptr,nullptr,&cancelled)==PROGRESS_CANCEL);
    puts("PASS: temporary-copy RPCS3 protocol, quoting, original retention, failure and missing output");
}
'''.replace('@LAUNCHER@', (ROOT / 'launcher/launcher.cpp').as_posix()))
(WORK / 'build.cmd').write_text(f'''@echo off
set "PATH=C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer;%PATH%"
call "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
rc /nologo /fo "{WORK}/launcher.res" "{WORK}/launcher.rc"
if errorlevel 1 exit /b 1
"C:\\Program Files\\LLVM\\bin\\clang-cl.exe" /nologo /std:c++20 /EHsc /O2 /MD /utf-8 /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0601 /I"{WORK}" /Fo"{WORK}/launcher.obj" /Fe"{WORK}/tourline5.exe" "{ROOT}/launcher/launcher.cpp" /link "{WORK}/launcher.res" /SUBSYSTEM:WINDOWS /MANIFEST:NO shell32.lib ole32.lib comctl32.lib bcrypt.lib uuid.lib user32.lib
if errorlevel 1 exit /b 1
"C:\\Program Files\\LLVM\\bin\\clang-cl.exe" /nologo /std:c++20 /EHsc /O2 /MD /D_CRT_SECURE_NO_WARNINGS /I"{ROOT}/src" /Fo"{WORK}/engine.obj" /Fe"{WORK}/gt5.exe" "{WORK}/engine.cpp"
if errorlevel 1 exit /b 1
"C:\\Program Files\\LLVM\\bin\\clang-cl.exe" /nologo /std:c++20 /EHsc /O2 /MD /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0601 /I"{WORK}" /Fo"{WORK}/decryption.obj" /Fe"{WORK}/decryption.exe" "{WORK}/decryption.cpp" /link shell32.lib ole32.lib comctl32.lib bcrypt.lib uuid.lib user32.lib
''')
subprocess.run(['cmd', '/c', str(WORK / 'build.cmd')], check=True)
(WORK / 'pt5-release.ini').write_text('[game]\ntarget=BCUS98114-v104\nengine_sha256=' + hashlib.sha256((WORK / 'gt5.exe').read_bytes()).hexdigest() + '\n')
(WORK / 'assets/glyphs').mkdir(parents=True)
(WORK / 'assets/glyphs/fixture.txt').write_text('fixture asset')


def sfo(path, **fields):
    keys = b''; values = b''; table = b''
    for key, value in fields.items():
        value = value.encode() + b'\0'
        table += struct.pack('<HHIII', len(keys), 0x204, len(value), len(value), len(values))
        keys += key.encode() + b'\0'; values += value
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(struct.pack('<IIIII', 0x46535000, 0x101, 20+len(table), 20+len(table)+len(keys), len(fields)) + table + keys + values)


disc = WORK / 'my disc & files/PS3_GAME'
sfo(disc / 'PARAM.SFO', TITLE_ID='BCUS98114', VERSION='02.00', APP_VER='01.00')
(disc / 'USRDIR').mkdir(); (disc / 'USRDIR/GT.VOL').write_bytes(b'synthetic volume')
installed = WORK / 'RPCS3 folder/dev_hdd0/game/BCUS98114'
sfo(installed / 'PARAM.SFO', TITLE_ID='BCUS98114', APP_VER='01.04')
(installed / 'USRDIR/PDIPFS/00').mkdir(parents=True)
(installed / 'USRDIR/PDIPFS/00/00').write_bytes(b'synthetic installed file')
(installed / 'USRDIR/installed00.dat').write_bytes(b'installed')
(installed / 'USRDIR/EBOOT.BIN').write_bytes(b'synthetic signed input')
executable = WORK / 'my executable.elf'; executable.write_bytes(ELF)
home = WORK / 'launcher home'; home.mkdir()
base_env = {**os.environ, 'PT5_LAUNCHER_HOME': str(home), 'PT5_SETUP_UI': '0'}
for key in ('PT5_USER_DIR', 'PT5_STORAGE_LINK', 'GT5P_ROOT', 'GT5P_GAME_DIR', 'GT5P_SETTINGS', 'RSX_SHADER_CACHE', 'GT5P_NO_SETTINGS'):
    base_env.pop(key, None)
count = WORK / 'child-count.txt'; base_env['PT5_TEST_COUNT'] = str(count)
subprocess.run([str(WORK/'decryption.exe'), str(WORK)], env={**base_env, 'PT5_TEST_DECRYPT_SOURCE':str(executable)}, check=True, timeout=30)


def run(*args, success=True, extra=None):
    result = subprocess.run([str(WORK / 'tourline5.exe'), *map(str, args)], env={**base_env, **(extra or {})},
                            cwd=WORK, capture_output=True, timeout=30)
    assert (result.returncode == 0) == success, (args, result.returncode, result.stderr.decode(errors='replace'))
    return result


def configure(source=disc.parent, data=installed.parents[2], elf=executable, success=True):
    return run('--configure', '--game', source, '--game-data', data, '--elf', elf, success=success)


def snapshot(folder):
    return {str(p.relative_to(folder)): hashlib.sha256(p.read_bytes()).hexdigest() for p in folder.rglob('*') if p.is_file()}


original_disc = snapshot(disc); original_data = snapshot(installed)
run('--check', success=False)
configure(); run('--check')
assert not count.exists(), 'Setup/check must not execute guest code'
assert (home / 'input/EMAIN.ELF').read_bytes() == ELF
assert (home / 'gamedata/dev_hdd0/game/BCUS98114/USRDIR/PDIPFS/00/00').read_bytes() == b'synthetic installed file'
assert snapshot(disc) == original_disc and snapshot(installed) == original_data
print('PASS: new setup, folder normalization, exact executable, source preservation, check without launch')

run()
log = (home / 'logs/play.out').read_text()
assert 'GT5P_ROOT=' + str(home) in log and 'GT5P_GAME_DIR=' + str(disc) + '/' in log
assert 'cwd=' + str(home) in log
assert 'RSX_SHADER_CACHE=' + str(home / 'shadercache') in log
assert 'GT5P_LAUNCHER_RESTART=1' in log
assert count.read_text() == 'run\n'
settings = home / 'gt5p_settings.toml'; settings.write_text('[video]\nmsaa=4\nshader_warmup=false\n')
restart = WORK / 'restart.marker'
run(extra={'PT5_TEST_RESTART': str(restart)})
assert restart.exists() and count.read_text() == 'run\nrun\n'
assert settings.read_text() == '[video]\nmsaa=4\nshader_warmup=false\n'
print('PASS: inherited paths, settings preservation, managed restart and child ownership')

second_disc = WORK/'another valid disc'; shutil.copytree(disc, second_disc)
configure(source=second_disc); run('--check'); run()
assert 'GT5P_GAME_DIR=' + str(second_disc) + '/' in (home/'logs/play.out').read_text()
assert settings.read_text() == '[video]\nmsaa=4\nshader_warmup=false\n'
print('PASS: changing selected game files is remembered without replacing settings')

before = (home / 'setup.ini').read_bytes()
bad_elf = WORK / 'wrong.elf'; bad_elf.write_bytes(b'wrong build')
configure(elf=bad_elf, success=False)
assert (home / 'setup.ini').read_bytes() == before
wrong = WORK / 'wrong disc'; shutil.copytree(disc, wrong)
sfo(wrong / 'PARAM.SFO', TITLE_ID='BCUS98158', VERSION='03.00')
configure(source=wrong, success=False)
wrong_data = WORK / 'wrong update'; shutil.copytree(installed, wrong_data)
sfo(wrong_data / 'PARAM.SFO', TITLE_ID='BCUS98114', APP_VER='02.00')
configure(data=wrong_data, success=False)
malformed = WORK / 'malformed disc'; malformed.mkdir(); (malformed/'PARAM.SFO').write_bytes(b'\0PSF')
configure(source=malformed, success=False)
run('--check')
assert (home / 'setup.ini').read_bytes() == before
print('PASS: wrong region/build/update and malformed SFO leave previous setup usable')

save = home / 'gamedata/dev_hdd0/home/00000001/savedata/test/save.txt'
save.parent.mkdir(parents=True); save.write_bytes(b'preserve my save')
cache = home / 'shadercache'; cache.mkdir(); (cache / 'fixture-cache').write_bytes(b'cache')
new_storage = WORK / 'External SSD storage'
run('--set-storage', new_storage); run('--check')
link = home/'selected-storage'
assert link.is_junction() and os.path.samefile(link, new_storage)
assert (new_storage / save.relative_to(home)).read_bytes() == b'preserve my save'
assert (new_storage / 'shadercache/fixture-cache').read_bytes() == b'cache'
assert settings.read_bytes() == (new_storage / 'gt5p_settings.toml').read_bytes()
assert save.exists(), 'Original storage must remain as a backup'
run()
assert 'GT5P_ROOT=' + str(new_storage) in (new_storage / 'logs/play.out').read_text()
locator = (home / 'storage.ini').read_bytes()
run('--set-storage', str(new_storage).upper())
assert (home / 'storage.ini').read_bytes() == locator
run('--check', '--setup', success=False)
run('--open-data', '--choose-storage', success=False)
occupied = WORK / 'occupied'; occupied.mkdir(); (occupied / 'keep').write_text('keep')
run('--set-storage', occupied, success=False)
run('--set-storage', new_storage / 'nested', success=False)
assert (home / 'storage.ini').read_bytes() == locator and (occupied / 'keep').read_text() == 'keep'
missing = WORK / 'disconnected drive'; new_storage.rename(missing)
run('--check', success=False)
assert (home / 'storage.ini').read_bytes() == locator and not new_storage.exists()
missing.rename(new_storage); run('--check')
print('PASS: verified storage migration, save/cache retention, occupied/nested destination refusal, disconnected drive retention')

# RecompLauncher's static manifest follows remembered storage, including old profiles.
link.rmdir()
run('--check')
assert not link.exists(), '--check must remain read-only'
run()
assert link.is_junction() and os.path.samefile(link, new_storage)
assert (link/'gt5p_settings.toml').read_bytes() == settings.read_bytes()
link.rmdir()
run(extra={'PT5_STORAGE_LINK': '0'})
assert not link.exists(), 'Rollback still created a link'
private_profile = WORK/'isolated profile'
private_profile.mkdir()
run('--configure', '--game', disc, '--game-data', installed, '--elf', executable,
    extra={'PT5_USER_DIR': str(private_profile)})
assert not link.exists(), 'PT5_USER_DIR must not redirect remembered storage'
link.mkdir()
(link/'keep.txt').write_text('real folder')
run()
assert (link/'keep.txt').read_text() == 'real folder'
assert 'not a junction' in (new_storage/'logs/storage-link.err').read_text()
(link/'keep.txt').unlink(); link.rmdir()
run()
assert link.is_junction() and os.path.samefile(link, new_storage)
locator_bytes = (home/'storage.ini').read_bytes()
(home/'storage.ini').unlink()
run()
assert not link.exists() and (new_storage/'gt5p_settings.toml').exists()
(home/'storage.ini').write_bytes(locator_bytes)
run()
assert link.is_junction() and os.path.samefile(link, new_storage)
print('PASS: RecompLauncher storage link, startup repair/removal, read-only check, override and rollback')

# Fail inside CopyFileEx and at the final configuration commit, after file moves.
# The latter exercises restoration of the previous imported game and executable.
kernel = ctypes.WinDLL('kernel32', use_last_error=True)
kernel.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p,
                              ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
kernel.CreateFileW.restype = ctypes.c_void_p
kernel.CloseHandle.argtypes = [ctypes.c_void_p]
locked = kernel.CreateFileW(str(installed/'USRDIR/PDIPFS/00/00'), 0x80000000, 0, None, 3, 0, None)
assert locked not in (None, ctypes.c_void_p(-1).value)
try:
    configure(success=False)
finally:
    kernel.CloseHandle(locked)
setup_before = (new_storage/'setup.ini').read_bytes()
new_update = WORK/'another valid installation'; shutil.copytree(installed, new_update)
(new_update/'USRDIR/PDIPFS/00/00').write_bytes(b'replacement data')
locked = kernel.CreateFileW(str(new_storage/'setup.ini'), 0x80000000, 1, None, 3, 0, None)
assert locked not in (None, ctypes.c_void_p(-1).value)
try:
    configure(data=new_update, success=False)
finally:
    kernel.CloseHandle(locked)
assert (new_storage/'setup.ini').read_bytes() == setup_before
assert (new_storage/'input/EMAIN.ELF').read_bytes() == ELF
assert (new_storage/'gamedata/dev_hdd0/game/BCUS98114/USRDIR/PDIPFS/00/00').read_bytes() == b'synthetic installed file'
run('--check')
print('PASS: copy failure and final commit failure preserve and restore the previous playable setup')

linked_update = WORK/'linked installation'; shutil.copytree(installed, linked_update)
outside = WORK/'outside'; outside.mkdir(); (outside/'keep').write_bytes(b'leave alone')
subprocess.run(['cmd','/c','mklink','/J',str(linked_update/'USRDIR/link'),str(outside)], check=True, capture_output=True)
configure(data=linked_update, success=False)
assert (outside/'keep').read_bytes() == b'leave alone'
assert (new_storage/'setup.ini').read_bytes() == setup_before
print('PASS: directory junctions are refused without traversing or altering their targets')

hold = WORK / 'hold.marker'
child = subprocess.Popen([str(WORK/'tourline5.exe')], env={**base_env, 'PT5_TEST_HOLD': str(hold)})
try:
    for _ in range(100):
        if hold.exists(): break
        time.sleep(.02)
    assert hold.exists()
    run('--set-storage', WORK / 'locked destination', success=False)
    run(success=False)
finally:
    child.wait(timeout=10)
print('PASS: active launcher owns storage; no second launch or migration while playing')

engine = WORK / 'gt5.exe'; raw = engine.read_bytes(); engine.write_bytes(raw + b'changed')
setup_before = (new_storage/'setup.ini').read_bytes()
run('--check', success=False); configure(success=False)
assert (new_storage/'setup.ini').read_bytes() == setup_before
engine.write_bytes(raw)
elf = new_storage / 'input/EMAIN.ELF'; elf.write_bytes(b'changed'); run('--check', success=False); elf.write_bytes(ELF)
run('--check')
assert snapshot(disc) == original_disc and snapshot(installed) == original_data
print('PASS: engine/executable tampering rejected; sources remain unchanged')
print('Evidence:', WORK)
