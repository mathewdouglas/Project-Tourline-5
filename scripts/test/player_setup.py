"""Production launcher integration using synthetic game files and a fixture engine.

No game process/data, RPCS3 installation, GPU or real user profile is touched.
"""
import ctypes
import hashlib
import json
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
ELF211 = b'\x7fELF synthetic 2.11 executable for setup tests\n'
SHA211 = hashlib.sha256(ELF211).hexdigest()
PKG = b'synthetic package'
def write_config(rpcs3_sha='0' * 64, rpcs3_bytes=0):
    # The setup-owned RPCS3 is pinned to a local fixture archive; tests never download.
    (WORK / 'pt5_launcher_config.h').write_text(f'#define PT5_ELF_SHA256 L"{SHA}"\n#define PT5_ELF_BYTES {len(ELF)}ull\n'
        f'#define PT5_DISC_PKG_SHA256 L"{hashlib.sha256(PKG).hexdigest()}"\n#define PT5_DISC_PKG_BYTES {len(PKG)}ull\n'
        '#define PT5_DISC_PKG_INSTALLED_FILES 0ull\n#define PT5_DISC_PKG_INSTALLED_BYTES 0ull\n'
        f'#define PT5_ELF_211_SHA256 L"{SHA211}"\n#define PT5_ELF_211_BYTES {len(ELF211)}ull\n'
        '#define PT5_RPCS3_URL L"https://invalid.example/rpcs3.7z"\n'
        f'#define PT5_RPCS3_SHA256 L"{rpcs3_sha}"\n#define PT5_RPCS3_BYTES {rpcs3_bytes}ull\n'
        '#define PT5_SEVENZIP_URL L"https://invalid.example/7zr.exe"\n'
        f'#define PT5_SEVENZIP_SHA256 L"{"0" * 64}"\n#define PT5_SEVENZIP_BYTES 0ull\n')


write_config()
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
    while (argc > 1 && (!strcmp(argv[1], "--allow-any-location") || !strcmp(argv[1], "--headless"))) {
        if (!strcmp(argv[1], "--headless")) { --argc; ++argv; continue; }
        if (const char* log=getenv("PT5_TEST_ANY_LOCATION_LOG")) std::ofstream(log,std::ios::app)<<argv[2]<<"\n";
        --argc; ++argv;
    }
    if (argc == 3 && !strcmp(argv[1], "--installpkg")) {
        if (const char* log=getenv("PT5_TEST_INSTALL_LOG")) std::ofstream(log)<<argv[2];
        if (getenv("PT5_TEST_INSTALL_FAIL")) return 8;
        if (const char* failure=getenv("PT5_TEST_CHAIN_FAIL"))
            if(std::filesystem::path(argv[2]).filename()==failure) return 8;
        if (!getenv("PT5_TEST_INSTALL_CANCEL")) {
            auto target=std::filesystem::current_path()/"dev_hdd0/game/BCUS98114";
            std::filesystem::create_directories(target.parent_path());
            auto source=std::filesystem::path(getenv("PT5_TEST_INSTALL_SOURCE"));
            if(const char* chain=getenv("PT5_TEST_CHAIN_SOURCE")) {
                auto step=std::filesystem::path(chain)/std::filesystem::path(argv[2]).stem();
                if(std::filesystem::is_directory(step)) source=step;
                if(const char* log=getenv("PT5_TEST_CHAIN_LOG")) std::ofstream(log,std::ios::app)<<std::filesystem::path(argv[2]).filename().string()<<"\n";
            }
            std::filesystem::copy(source,target,std::filesystem::copy_options::recursive|std::filesystem::copy_options::overwrite_existing);
        }
        if (const char* failure=getenv("PT5_TEST_HEADLESS_EXIT")) {
            if (!strcmp(failure,"known")) fputs("fixed_typemap.hpp:220: Emulation object is unavailable (process teardown)",stderr);
            else fputs("unrelated failure",stderr);
            return static_cast<int>(0xc0000409u);
        }
        return 0;
    }
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
    if(const char* p=getenv("PT5_TEST_SWITCH")) {
        std::ofstream(std::string(p)+".runs",std::ios::app)<<getenv("GT5P_GAME_VERSION")<<" "<<getenv("GT5P_ROOT")<<"\n";
        if(!std::filesystem::exists(p)) {std::ofstream(p)<<"switch";return pt5_restart::version_211;}
    }
    if(const char* p=getenv("PT5_TEST_MOD")) {   // the mod environment of each start; asks once for choice PT5_TEST_MOD_PICK
        auto v=[](const char* k){return std::string(getenv(k)?getenv(k):"<unset>");};
        std::ofstream(std::string(p)+".runs",std::ios::app)<<v("GT5P_MOD_CHOICES")<<"\t"<<v("GT5P_MOD_ACTIVE")<<"\t"<<v("PS3_FS_OVERLAY")<<"\n";
        if(!std::filesystem::exists(p)&&getenv("PT5_TEST_MOD_PICK")) {std::ofstream(p)<<"pick";return pt5_restart::mod_select|(unsigned long)atoi(getenv("PT5_TEST_MOD_PICK"));}
    }
    if(const char* p=getenv("PT5_TEST_COUNT")) {std::ofstream(p,std::ios::app)<<"run\n";}
    // Crash-report fixtures: the lines the real game writes, then its exit.
    char profile[512]="", user[128]="";
    GetEnvironmentVariableA("USERPROFILE",profile,sizeof profile); GetEnvironmentVariableA("USERNAME",user,sizeof user);
    if(getenv("PT5_TEST_CRASH")) {
        fprintf(stderr,"[milestone] RaceStart\nloading %s\\shadercache for %s\n",profile,user);
        fprintf(stderr,"\n[GT5P] ACCESS VIOLATION: read at host 0x10 (thread 5)\n[GT5P]   fault chain: func_00123456 func_00ABCDEF\n");
        fflush(stderr); ExitProcess(0xC0000005);
    }
    if(getenv("PT5_TEST_FREEZE")) {
        fprintf(stderr,"[milestone] RaceStart\n[stall] no present for 3001 ms after 900 presents\n[stall] no present for 11007 ms after 900 presents\n[stall] lockline holder tid=0 held 0 ms\n");
        return 0;
    }
    if(getenv("PT5_TEST_HITCH")) { fprintf(stderr,"[stall] no present for 3001 ms after 900 presents\n"); return 0; }
    if(const char* c=getenv("PT5_TEST_EXIT")) { fprintf(stderr,"fatal: something went wrong\n"); return atoi(c); }
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
"C:\\Program Files\\LLVM\\bin\\clang-cl.exe" /nologo /std:c++20 /EHsc /O2 /MD /utf-8 /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0601 /I"{WORK}" /Fo"{WORK}/launcher.obj" /Fe"{WORK}/tourline5.exe" "{ROOT}/launcher/launcher.cpp" /link "{WORK}/launcher.res" /SUBSYSTEM:WINDOWS /MANIFEST:NO shell32.lib ole32.lib oleaut32.lib comctl32.lib bcrypt.lib uuid.lib user32.lib winhttp.lib
if errorlevel 1 exit /b 1
"C:\\Program Files\\LLVM\\bin\\clang-cl.exe" /nologo /std:c++20 /EHsc /O2 /MD /D_CRT_SECURE_NO_WARNINGS /I"{ROOT}/src" /Fo"{WORK}/engine.obj" /Fe"{WORK}/gt5.exe" "{WORK}/engine.cpp"
if errorlevel 1 exit /b 1
"C:\\Program Files\\LLVM\\bin\\clang-cl.exe" /nologo /std:c++20 /EHsc /O2 /MD /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0601 /I"{WORK}" /Fo"{WORK}/decryption.obj" /Fe"{WORK}/decryption.exe" "{WORK}/decryption.cpp" /link "{WORK}/launcher.res" /MANIFEST:NO shell32.lib ole32.lib oleaut32.lib comctl32.lib bcrypt.lib uuid.lib user32.lib winhttp.lib
''')
subprocess.run(['cmd', '/c', str(WORK / 'build.cmd')], check=True)
# Setup-owned RPCS3 fixture: an archive holding the fixture engine as rpcs3.exe.
# Rebuild only the launcher with that archive pinned (the engine's hash must not change).
rpcs3_archive = WORK / 'pinned rpcs3.zip'
with tempfile.TemporaryDirectory(dir=WORK) as packing:
    shutil.copyfile(WORK / 'gt5.exe', Path(packing) / 'rpcs3.exe')
    shutil.make_archive(str(rpcs3_archive.with_suffix('')), 'zip', root_dir=packing, base_dir='rpcs3.exe')
write_config(hashlib.sha256(rpcs3_archive.read_bytes()).hexdigest(), rpcs3_archive.stat().st_size)
build_lines = (WORK / 'build.cmd').read_text().splitlines()
(WORK / 'build_launcher.cmd').write_text('\n'.join(build_lines[:4] + [l for l in build_lines if 'launcher.cpp' in l]) + '\n')
subprocess.run(['cmd', '/c', str(WORK / 'build_launcher.cmd')], check=True)
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
# PT5_RPCS3_DOWNLOAD=0 keeps the explicit-input checks below; the setup-owned route is tested separately.
base_env = {**os.environ, 'PT5_LAUNCHER_HOME': str(home), 'PT5_SETUP_UI': '0', 'PT5_RPCS3_DOWNLOAD': '0'}
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

# Drive preparation from the shipped manifest, including omitted optional arguments.
manifest = json.loads((ROOT/'launcher/recomplauncher.json').read_text())
assert manifest['prepare']['program'] == ['tourline5.exe']
easy_home = WORK/'Easy Setup profile'
easy_env = {'PT5_LAUNCHER_HOME': str(easy_home), 'PT5_SETUP_UI': '1'}


def prepare(update=installed.parents[2], optional=None, success=True, extra=None, source=disc.parent):
    selected = {'disc': str(source)}
    if update is not None:
        selected['update'] = str(update)
    if optional is not None:
        selected['executable'] = str(optional)
    args = []
    for argument in manifest['prepare']['args']:
        for key, value in selected.items():
            argument = argument.replace('{input:'+key+'}', value)
        if '{input:' not in argument:
            args.append(argument)
    before_runs = count.read_bytes() if count.exists() else b''
    result = run(*args, success=success, extra={**easy_env, **(extra or {})})
    assert (count.read_bytes() if count.exists() else b'') == before_runs, 'Preparation launched the game'
    progress = [float(line.split()[1]) for line in result.stdout.decode().splitlines() if line.startswith('progress ')]
    if success:
        assert progress and progress[-1] == 1 and progress == sorted(progress), result.stdout
        run('--check', extra=easy_env)
    else:
        assert 1 not in progress
    return result


prepare(optional=executable)
assert (easy_home/'input/EMAIN.ELF').read_bytes() == ELF
easy_settings = easy_home/'gt5p_settings.toml'
easy_settings.write_text('[video]\nmsaa=4\nshader_warmup=false\n')
before_setup = (easy_home/'setup.ini').read_bytes()
prepare(success=False)  # no inferred ELF or RPCS3: fail with an actionable error, never a dialog
assert (easy_home/'setup.ini').read_bytes() == before_setup
assert b'optional Executable / RPCS3 input' in prepare(success=False).stderr
(installed/'USRDIR/EBOOT.elf').write_bytes(ELF)
prepare()
(installed/'USRDIR/EBOOT.elf').unlink()
fake_rpcs3 = installed.parents[2]/'rpcs3.exe'
shutil.copyfile(WORK/'gt5.exe', fake_rpcs3)
prepare(extra={'PT5_TEST_DECRYPT_SOURCE': str(executable)})
prepare(update=installed, optional=fake_rpcs3, extra={'PT5_TEST_DECRYPT_SOURCE': str(executable)})
prepare(optional=fake_rpcs3, success=False,
        extra={'PT5_TEST_DECRYPT_SOURCE': str(executable), 'PT5_TEST_DECRYPT_FAIL': '1'})
prepare(optional=fake_rpcs3, success=False,
        extra={'PT5_TEST_DECRYPT_SOURCE': str(executable), 'PT5_TEST_DECRYPT_NO_OUTPUT': '1'})
wrong_easy = WORK/'wrong easy executable.elf'; wrong_easy.write_bytes(b'wrong build')
prepare(optional=wrong_easy, success=False)

# Disc + RPCS3: no installed-data picker, update download or game boot.
disc_rpc = WORK/'fresh RPCS3 & spaces'; disc_rpc.mkdir()
disc_rpc_exe = disc_rpc/'rpcs3.exe'; shutil.copyfile(WORK/'gt5.exe', disc_rpc_exe)
package = disc/'INSDIR/DATA000.PKG'; package.parent.mkdir(); package.write_bytes(PKG)
install_log = WORK/'package-argument.txt'
disc_env = {'PT5_TEST_INSTALL_SOURCE':str(installed), 'PT5_TEST_DECRYPT_SOURCE':str(executable),
            'PT5_TEST_INSTALL_LOG':str(install_log)}
prepare(update=None, optional=disc_rpc_exe, success=False, extra={**disc_env, 'PT5_TEST_INSTALL_FAIL':'1'})
prepare(update=None, optional=disc_rpc_exe, success=False, extra={**disc_env, 'PT5_TEST_INSTALL_CANCEL':'1'})
assert (easy_home/'setup.ini').read_bytes() == before_setup
prepare(update=None, optional=disc_rpc_exe, extra=disc_env)
assert os.path.samefile(install_log.read_text(), package)
assert (easy_home/'input/EMAIN.ELF').read_bytes() == ELF
install_log.unlink()
prepare(update=None, optional=disc_rpc_exe, extra=disc_env)  # reuse 01.04; do not reinstall
assert not install_log.exists()
disc_rpc_game = disc_rpc/'dev_hdd0/game/BCUS98114'
sfo(disc_rpc_game/'PARAM.SFO', TITLE_ID='BCUS98114', APP_VER='02.00')
newer_before = snapshot(disc_rpc_game)
prepare(update=None, optional=disc_rpc_exe, success=False, extra=disc_env)
assert snapshot(disc_rpc_game) == newer_before and not install_log.exists()
shutil.rmtree(disc_rpc_game)
package.write_bytes(b'corrupt package!!')
prepare(update=None, optional=disc_rpc_exe, success=False, extra=disc_env)
assert not install_log.exists()
package.write_bytes(PKG)
(disc_rpc/'config').mkdir()
(disc_rpc/'config/vfs.yml').write_text('/dev_hdd0/: another folder\n')
prepare(update=None, optional=disc_rpc_exe, success=False, extra=disc_env)
assert not install_log.exists()
(disc_rpc/'config/vfs.yml').unlink()
# The built-in launcher's command-line route accepts the same two inputs.
run('--configure', '--game', disc.parent, '--elf', disc_rpc_exe,
    extra={**easy_env, **disc_env})
run('--check', extra=easy_env)
assert install_log.exists()
install_log.unlink()
shutil.rmtree(disc_rpc_game)
# Disc only: setup unpacks its own pinned RPCS3, installs, decrypts, imports, then removes it.
any_location = WORK/'any-location.txt'
managed_env = {**disc_env, 'PT5_RPCS3_DOWNLOAD': '1', 'PT5_RPCS3_ARCHIVE': str(rpcs3_archive),
               'PT5_TEST_ANY_LOCATION_LOG': str(any_location)}
kept_settings = easy_settings.read_text(); easy_settings.write_text('[video]\nmsaa=8\n')
prepare(update=None, extra=managed_env)
assert os.path.samefile(install_log.read_text(), package)
assert any_location.read_text().split() == ['--installpkg', '--decrypt'], any_location.read_text()
assert (easy_home/'input/EMAIN.ELF').read_bytes() == ELF and not (easy_home/'tools').exists()
assert easy_settings.read_text() == '[video]\nmsaa=8\n'
install_log.unlink(); any_location.unlink()
# A kept tool is reused without unpacking again; a changed archive is refused before RPCS3 starts.
prepare(update=None, extra={**managed_env, 'PT5_KEEP_RPCS3': '1'})
assert (easy_home/'tools/rpcs3/rpcs3.exe').exists()
(easy_home/'logs/unpack-rpcs3.out').unlink()
shutil.rmtree(easy_home/'tools/rpcs3/dev_hdd0')
prepare(update=None, extra={**managed_env, 'PT5_KEEP_RPCS3': '1'})
assert not (easy_home/'logs/unpack-rpcs3.out').exists()
shutil.rmtree(easy_home/'tools'); install_log.unlink()
wrong_archive = WORK/'wrong rpcs3.zip'; wrong_archive.write_bytes(b'not the pinned build')
assert b'not the pinned RPCS3' in prepare(update=None, success=False,
    extra={**managed_env, 'PT5_RPCS3_ARCHIVE': str(wrong_archive)}).stderr
assert not install_log.exists()
# The command-line route takes the disc alone too.
run('--configure', '--game', disc.parent, extra={**easy_env, **managed_env})
run('--check', extra=easy_env)
assert not (easy_home/'tools').exists()
install_log.unlink(); easy_settings.write_text(kept_settings)
print('PASS: disc-only setup with a setup-owned pinned RPCS3, reuse, mismatch refusal and removal')
package.unlink()
missing_package = prepare(update=None, optional=disc_rpc_exe, success=False, extra=disc_env)
assert b'INSDIR' in missing_package.stderr and not install_log.exists()
prepare(update=None, success=False)
prepare(update=None, optional=executable, success=False)
print('PASS: disc package handoff, automatic decrypt/import, reuse, cancel/failure, missing package and newer-version protection')
assert b'does not match GT5' in prepare(optional=fake_rpcs3, success=False,
        extra={'PT5_TEST_DECRYPT_SOURCE': str(wrong_easy)}).stderr
fake_rpcs3.unlink()
prepare(optional=WORK/'gt5.exe', success=False)
prepare(optional=executable, source=WORK/'missing disc', success=False)
prepare(update=WORK/'missing update', optional=executable, success=False)
assert (easy_home/'setup.ini').read_bytes() == before_setup
assert (easy_home/'input/EMAIN.ELF').read_bytes() == ELF
assert easy_settings.read_text() == '[video]\nmsaa=4\nshader_warmup=false\n'
assert snapshot(disc) == original_disc and snapshot(installed) == original_data
run(extra=easy_env)  # ordinary Play after preparation still uses the native launch/restart owner
assert 'GT5P_ROOT='+str(easy_home) in (easy_home/'logs/play.out').read_text()
easy_storage = WORK/'Easy Setup external storage'
run('--set-storage', easy_storage, extra=easy_env)
prepare(optional=executable)
assert (easy_storage/'input/EMAIN.ELF').read_bytes() == ELF
assert os.path.samefile(easy_home/'selected-storage', easy_storage)
assert easy_settings.read_bytes() == (easy_storage/'gt5p_settings.toml').read_bytes()
count.unlink()
print('PASS: manifest Easy Setup, optional/automatic ELF/RPCS3, progress, no game launch/dialogs, failure retention and native Play')

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

# Crash reports (launcher/crash_report.h). PT5_SETUP_UI=0 saves without a dialog.
import urllib.parse
import zipfile
reports = new_storage / 'logs/reports'
url_file = WORK / 'issue-url.txt'


def report_run(extra, success, kind):
    before = set(reports.glob('*.zip')) if reports.exists() else set()
    url_file.unlink(missing_ok=True)
    run(success=success, extra={'PT5_CRASH_REPORT_URL_FILE': str(url_file), **extra})
    made = sorted(set(reports.glob('*.zip')) - before) if reports.exists() else []
    if kind is None:
        assert not made and not url_file.exists(), made
        return None
    assert len(made) == 1 and made[0].name.startswith(kind + '-'), made
    return made[0]


def secrets():
    return [s for s in (os.environ.get('USERPROFILE'), os.environ.get('USERNAME'), os.environ.get('COMPUTERNAME')) if s]


crash = report_run({'PT5_TEST_CRASH': '1'}, False, 'crash')
with zipfile.ZipFile(crash) as z:
    assert z.testzip() is None
    names = set(z.namelist())
    assert {'README.txt', 'summary.md', 'system.txt', 'play.err', 'play.out', 'gt5p_settings.toml', 'setup.ini'} <= names, names
    raw = b''.join(z.read(n) for n in names)
    err = z.read('play.err').decode()
    assert '%USERPROFILE%' in z.read('setup.ini').decode('utf-8'), 'setup.ini must be UTF-8 and scrubbed'
for s in secrets():
    for enc in ('utf-8', 'utf-16-le'):
        assert s.lower().encode(enc) not in raw.lower(), f'personal name left in report ({enc}): ' + s
assert '%USERPROFILE%\\shadercache for <user>' in err, err
url = url_file.read_text()
assert url.startswith('https://github.com/mathewdouglas/Project-Tourline-5/issues/new?title=') and len(url) <= 7000
query = urllib.parse.parse_qs(urllib.parse.urlsplit(url).query)
assert query['title'] == ['Crash: crashed (access violation, 0xC0000005)'], query['title']
body = query['body'][0]
assert 'func_00123456' in body and '[milestone] RaceStart' in body and crash.name in body
for s in secrets():
    assert s.lower() not in url.lower()
print('PASS: crash: report saved, names scrubbed, issue link carries the fault chain')

freeze = report_run({'PT5_TEST_FREEZE': '1'}, True, 'freeze')
query = urllib.parse.parse_qs(urllib.parse.urlsplit(url_file.read_text()).query)
assert query['title'] == ['Freeze: froze (no new picture for 11 s)'], query['title']
report_run({'PT5_TEST_HITCH': '1'}, True, None)
report_run({'PT5_TEST_FREEZE': '1', 'PT5_CRASH_FREEZE_MS': '20000'}, True, None)
print('PASS: freeze of 11 s reported; a 3 s hitch and a raised threshold are not')

report_run({'PT5_TEST_EXIT': '3'}, False, 'error')
query = urllib.parse.parse_qs(urllib.parse.urlsplit(url_file.read_text()).query)
assert query['title'] == ['Error exit: exited with code 3 (abort) (0x00000003)'], query['title']
assert 'fatal: something went wrong' in query['body'][0]
report_run({'PT5_TEST_CRASH': '1', 'PT5_CRASH_REPORT': '0'}, False, None)
report_run({}, True, None)
print('PASS: error exit reported; PT5_CRASH_REPORT=0 and a clean exit make no report')
print('Evidence:', WORK)

# Both versions coexist, and a managed F1-style request crosses engine/root.
shutil.copyfile(WORK/'gt5.exe', WORK/'gt5_211.exe')
with (WORK/'pt5-release.ini').open('a') as f:
    f.write('\n[BCUS98114-v211]\nengine_sha256='+hashlib.sha256((WORK/'gt5_211.exe').read_bytes()).hexdigest()+'\nvolmap=1\n')
installed211=WORK/'installed 2.11'
shutil.copytree(installed,installed211)
sfo(installed211/'PARAM.SFO',TITLE_ID='BCUS98114',APP_VER='02.11')
elf211=WORK/'update211.elf';elf211.write_bytes(ELF211)
both_home=WORK/'both versions'
env_both={'PT5_LAUNCHER_HOME':str(both_home)}
run('--configure','--game',disc,'--game-data',installed,'--elf',executable,extra=env_both)
run('--configure','--game',disc,'--game-data',installed211,'--elf',elf211,extra=env_both)
run('--check','--target=01.04',extra=env_both)
run('--check','--target=02.11',extra=env_both)
assert (both_home/'input/EMAIN.ELF').read_bytes()==ELF
assert (both_home/'targets/BCUS98114-v211/input/EMAIN.ELF').read_bytes()==ELF211
request=WORK/'version-switch'
run('--target=01.04',extra={**env_both,'PT5_TEST_SWITCH':str(request)})
launches=Path(str(request)+'.runs').read_text().splitlines()
assert len(launches)==2 and launches[0].startswith('01.04 ') and launches[1].startswith('02.11 '),launches
assert 'targets' in launches[1] and 'targets' not in launches[0]
run('--target=02.17','--check',success=False,extra=env_both)
# Wrong executable must leave both installed versions and remembered selection usable.
before104=snapshot(both_home/'gamedata');before211=snapshot(both_home/'targets')
run('--configure','--target=02.11','--game',disc,'--game-data',installed211,'--elf',executable,success=False,extra=env_both)
assert before104==snapshot(both_home/'gamedata') and before211==snapshot(both_home/'targets')
run('--check',extra=env_both)
print('PASS: dual-version import, identity rejection, separate roots, managed version switch and 2.17 refusal')

# Copying the 01.04 save to 2.11 (offer before 2.11 starts; --copy-104-save).
save104=both_home/'gamedata/dev_hdd0/home/00000001/savedata'
save211=both_home/'targets/BCUS98114-v211/gamedata/dev_hdd0/home/00000001/savedata'
answer=both_home/'targets/BCUS98114-v211/save-copy.ini'
for d,payload in (('BCUS98114-GAME',b'01.04 progress'),('BCUS98114-BKUP',b'01.04 backup')):
    (save104/d).mkdir(parents=True,exist_ok=True)
    sfo(save104/d/'PARAM.SFO',TITLE_ID='BCUS98114')
    (save104/d/'GT5.0').write_bytes(payload)
original104=snapshot(save104)
run('--target=02.11',extra=env_both)                       # silent run: never asks, records nothing
assert not save211.exists() and not answer.exists()
run('--target=02.11',extra={**env_both,'PT5_SAVE_COPY':'0'})
assert not save211.exists() and not answer.exists()
run('--target=01.04',extra={**env_both,'PT5_SAVE_COPY':'1'})  # only before 2.11
assert not save211.exists() and not answer.exists()
run('--target=02.11',extra={**env_both,'PT5_SAVE_COPY':'1'})
assert (save211/'BCUS98114-GAME/GT5.0').read_bytes()==b'01.04 progress'
assert (save211/'BCUS98114-BKUP/GT5.0').read_bytes()==b'01.04 backup'
assert 'copied' in answer.read_text(encoding='utf-16')
assert snapshot(save104)==original104, '01.04 save must only be read'
assert not [p for p in save211.iterdir() if '.copy-' in p.name], 'no staging left behind'
# Answered once: a later launch does not copy again.
(save104/'BCUS98114-GAME/GT5.0').write_bytes(b'01.04 later')
(save211/'BCUS98114-GAME/GT5.0').write_bytes(b'2.11 progress')
run('--target=02.11',extra={**env_both,'PT5_SAVE_COPY':'1'})
assert (save211/'BCUS98114-GAME/GT5.0').read_bytes()==b'2.11 progress'
# --copy-104-save replaces 2.11's save and keeps it under backups/.
run('--copy-104-save','--target=02.11',success=False,extra=env_both)
run('--copy-104-save',extra=env_both)
assert (save211/'BCUS98114-GAME/GT5.0').read_bytes()==b'01.04 later'
kept=list((both_home/'backups').glob('save-211-*/00000001/BCUS98114-GAME/GT5.0'))
assert len(kept)==1 and kept[0].read_bytes()==b'2.11 progress', kept
nosave_home=WORK/'no 01.04 save'
run('--copy-104-save',success=False,extra={'PT5_LAUNCHER_HOME':str(nosave_home)})
print('PASS: 01.04 save copied to 2.11 once (PT5_SAVE_COPY=1), never when silent or =0, --copy-104-save keeps a backup')

# Data mods (launcher/mods.h): install from a ZIP, refuse other bases, select, overlay, remove.
import zipfile
pdipfs211=both_home/'targets/BCUS98114-v211/gamedata/dev_hdd0/game/BCUS98114/USRDIR/PDIPFS'
(pdipfs211/'K').mkdir(parents=True,exist_ok=True)
(pdipfs211/'K/4D').write_bytes(b'original volume header')
installed_before=snapshot(pdipfs211)
def mod_zip(name,entries):
    z=WORK/name
    with zipfile.ZipFile(z,'w',zipfile.ZIP_DEFLATED) as f:
        for n,d in entries.items(): f.writestr(n,d)
    return z
good=mod_zip('GT5_2.11_Synthetic_Mod_1.0_(US-BCUS98114).zip',{
    'PDIPFS/K/4D':b'modded header','PDIPFS/9/AA/BB':b'mod file',
    'Debug_Cam_Addon/PDIPFS/K/4D':b'addon header',
    'PDIPFS_ORIGINAL/K/4D':b'original volume header','readme.txt':b'not kept'})
other=mod_zip('Other_Region_Mod.zip',{'PDIPFS/K/4D':b'x','PDIPFS_ORIGINAL/K/4D':b'another region header'})
nodata=mod_zip('Not_A_Mod.zip',{'readme.txt':b'no PDIPFS here'})
escape=mod_zip('Escape_Mod.zip',{'PDIPFS/K/4D':b'x','../escaped.txt':b'outside'})
mods_root=both_home/'targets/BCUS98114-v211/mods'
run('--install-mod',other,'--target=02.11',success=False,extra=env_both)
run('--install-mod',nodata,'--target=02.11',success=False,extra=env_both)
run('--install-mod',escape,'--target=02.11',success=False,extra=env_both)
assert not (WORK/'escaped.txt').exists() and not (mods_root.parent/'escaped.txt').exists()
assert not [p for p in mods_root.iterdir() if p.is_dir()], 'refused mods leave nothing behind'
run('--install-mod',good,'--target=02.11',extra=env_both)
mod=mods_root/'synthetic-mod-1.0'
assert (mod/'PDIPFS/K/4D').read_bytes()==b'modded header' and (mod/'PDIPFS/9/AA/BB').read_bytes()==b'mod file'
assert (mod/'addons/Debug_Cam_Addon/PDIPFS/K/4D').read_bytes()==b'addon header'
assert not (mod/'readme.txt').exists() and not (mod/'PDIPFS_ORIGINAL').exists()
assert sorted(p.name for p in mods_root.iterdir())==['mods.ini','synthetic-mod-1.0'], list(mods_root.iterdir())
assert snapshot(pdipfs211)==installed_before, 'installed data must not change'
assert 'synthetic-mod-1.0' in (mods_root/'mods.ini').read_text(encoding='utf-16')
backups=list((both_home/'backups').glob('save-before-mod-*/00000001/BCUS98114-GAME/GT5.0'))
assert len(backups)==1, backups   # turned on from Off: the save was copied first
run('--install-mod',good,'--target=02.11',success=False,extra=env_both)   # already installed
# Each start gets the choices and overlay; F1's choice (exit code) is kept for the next start.
pick=WORK/'mod-pick'
run('--target=02.11',extra={**env_both,'PT5_TEST_MOD':str(pick),'PT5_TEST_MOD_PICK':'2'})
starts=[l.split('\t') for l in Path(str(pick)+'.runs').read_text().splitlines()]
assert starts[0]==['Off|Synthetic Mod 1.0|Synthetic Mod + Debug Cam','1',str(mod)], starts
assert starts[1]==['Off|Synthetic Mod 1.0|Synthetic Mod + Debug Cam','2',str(mod/'addons/Debug_Cam_Addon')+';'+str(mod)], starts
assert len(starts)==2
assert len(list((both_home/'backups').glob('save-before-mod-*')))==1, 'switching between mods copies no save'
# PT5_MODS=0 loads nothing and offers nothing; a developer's own overlay wins.
for extra,want in (({'PT5_MODS':'0'},['<unset>','0','<unset>']),({'PS3_FS_OVERLAY':'C:\\dev overlay'},None)):
    log=WORK/('mod-env-'+'-'.join(extra));Path(str(log)+'.runs').unlink(missing_ok=True)
    run('--target=02.11',extra={**env_both,**extra,'PT5_TEST_MOD':str(log)})
    got=Path(str(log)+'.runs').read_text().splitlines()[0].split('\t')
    assert (got==want) if want else got[2]=='C:\\dev overlay', got
run('--target=01.04',extra={**env_both,'PT5_TEST_MOD':str(WORK/'mod-104')})
got=Path(str(WORK/'mod-104')+'.runs').read_text().splitlines()[0].split('\t')
assert got==['Off','0','<unset>'], got   # mods belong to a version
run('--remove-mod','no-such-mod','--target=02.11',success=False,extra=env_both)
run('--remove-mod','synthetic-mod-1.0','--target=02.11',extra=env_both)
assert not mod.exists() and snapshot(pdipfs211)==installed_before
log=WORK/'mod-after-remove'
run('--target=02.11',extra={**env_both,'PT5_TEST_MOD':str(log)})
assert Path(str(log)+'.runs').read_text().splitlines()[0].split('\t')==['Off','0','<unset>']
print('PASS: mods: ZIP install (PDIPFS only, add-on), other base/no data/path escape refused, save copied when turned on, F1 choice kept, PT5_MODS=0, overlay override, per version, remove')

# Run the production package-chain installer with tiny synthetic package pins.
# Every stage is handled by the fixture RPCS3, never by a real emulator.
(disc/'INSDIR').mkdir(exist_ok=True)
(disc/'INSDIR/DATA000.PKG').write_bytes(PKG)
chain_folder=WORK/'chain packages';chain_folder.mkdir()
chain_source=WORK/'chain contents';chain_source.mkdir()
base_source=WORK/'chain base';shutil.copytree(installed,base_source)
for unwanted in ('EBOOT.elf','EBOOT.ELF','EMAIN.ELF'):
    (base_source/'USRDIR'/unwanted).unlink(missing_ok=True)
rows=[]
for n in range(21):
    version=('01.'+str(n+5).zfill(2)) if n<9 else ('02.'+str(n-9).zfill(2))
    name='fixture-'+version+'.pkg'
    payload=('fixture package '+version).encode()
    (chain_folder/name).write_bytes(payload+hashlib.sha1(payload).digest()+b'\0'*12)
    stage=chain_source/Path(name).stem;shutil.copytree(base_source,stage)
    sfo(stage/'PARAM.SFO',TITLE_ID='BCUS98114',APP_VER=version)
    inventory=[p for p in stage.rglob('*') if p.is_file()]
    rows.append('{L"'+version+'",L"'+name+'",L"'+hashlib.sha1(payload).hexdigest()+'",'+str(len(payload)+32)+'ull,'+str(len(inventory))+'ull,'+str(sum(p.stat().st_size for p in inventory))+'ull}')
(WORK/'fixture-updates.h').write_text('struct UpdatePackage { const wchar_t *version,*file,*sha1; unsigned long long bytes,files,installed_bytes; };\nstatic const UpdatePackage update_packages[]={'+','.join(rows)+'};\n')
with (WORK/'pt5_launcher_config.h').open('a') as f:
    f.write('#define PT5_UPDATE_PACKAGES_HEADER "fixture-updates.h"\n')
    base_files=[p for p in base_source.rglob('*') if p.is_file()]
    f.write('#undef PT5_DISC_PKG_INSTALLED_FILES\n#undef PT5_DISC_PKG_INSTALLED_BYTES\n'
            f'#define PT5_DISC_PKG_INSTALLED_FILES {len(base_files)}ull\n'
            f'#define PT5_DISC_PKG_INSTALLED_BYTES {sum(p.stat().st_size for p in base_files)}ull\n')
subprocess.run(['cmd','/c',str(WORK/'build_launcher.cmd')],check=True)
chain_home=WORK/'chain profile'
chain_log=WORK/'chain log'
chain_env={**env_both,'PT5_LAUNCHER_HOME':str(chain_home),'PT5_RPCS3_DOWNLOAD':'1',
           'PT5_RPCS3_ARCHIVE':str(rpcs3_archive),'PT5_KEEP_RPCS3':'1',
           'PT5_TEST_INSTALL_SOURCE':str(base_source),'PT5_TEST_CHAIN_SOURCE':str(chain_source),
           'PT5_TEST_CHAIN_LOG':str(chain_log),'PT5_TEST_DECRYPT_SOURCE':str(elf211)}
# Preserve an existing 01.04 profile across update failure and retry.
run('--configure','--game',disc,'--game-data',installed,'--elf',executable,extra=chain_env)
original=snapshot(chain_home/'gamedata')
chain_args=('--recomplauncher-prepare','--game',disc,'--updates='+str(chain_folder))
run(*chain_args,success=False,extra={**chain_env,'PT5_TEST_CHAIN_FAIL':'fixture-02.03.pkg'})
assert original==snapshot(chain_home/'gamedata')
assert not (chain_home/'targets/BCUS98114-v211/setup.ini').exists()
assert (chain_home/'tools-v211').is_dir()
run(*chain_args,extra=chain_env)
assert original==snapshot(chain_home/'gamedata')
run('--check','--target=02.11',extra=chain_env)
log=chain_log.read_text().splitlines()
assert log.count('fixture-01.05.pkg')==1,log # completed stages reused on retry
assert log.count('fixture-02.11.pkg')==1,log
# A malformed package is rejected before the isolated tool is touched.
last=chain_folder/'fixture-02.11.pkg';good=last.read_bytes();last.write_bytes(b'broken')
before_log=chain_log.read_bytes()
run(*chain_args,success=False,extra=chain_env)
assert chain_log.read_bytes()==before_log
last.write_bytes(good)
run('--check',extra=chain_env)
print('PASS: 21-package validation/install, failed-stage retention, resume, 01.04 preservation and malformed package rejection')

# A complete extraction alone must not hide an unrelated tool failure.
unknown_home=WORK/'unknown headless failure'
unknown={**chain_env,'PT5_LAUNCHER_HOME':str(unknown_home),'PT5_TEST_HEADLESS_EXIT':'unknown'}
run(*chain_args,success=False,extra=unknown)
assert not (unknown_home/'targets/BCUS98114-v211/setup.ini').exists()
# The pinned teardown signature is accepted only together with complete output.
known_home=WORK/'known headless teardown'
known={**chain_env,'PT5_LAUNCHER_HOME':str(known_home),'PT5_TEST_HEADLESS_EXIT':'known'}
run(*chain_args,extra=known)
run('--check','--target=02.11',extra=known)
print('PASS: complete output with unrelated tool failure rejected; exact pinned headless teardown accepted')
