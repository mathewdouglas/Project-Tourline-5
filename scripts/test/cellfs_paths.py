"""Compile the production cellFs translator and check split-storage mappings."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
runtime = Path(os.environ.get('PS3RECOMP_DIR', root.parent / 'ps3recomp-tourline5'))
folder = Path(tempfile.mkdtemp(prefix='cellfs-paths-', dir=root / 'scratch'))
source = (runtime / 'libs/filesystem/cellFs.c').read_text()
start = source.index('#define MAX_PATH_MAPPINGS')
end = source.index('/* Public wrapper for translate_path', start)
harness = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CELL_FS_MAX_FS_PATH_LENGTH 1024
void cellfs_add_path_mapping(const char*, const char*);
void ps3_vfs_ps3game_fallback(char* path, size_t cap) { (void)path; (void)cap; }
'''
harness += source[start:end]
harness += r'''
static void check(const char* guest, const char* expected) {
    char actual[1024];
    if (translate_path(guest, actual, sizeof actual) || strcmp(actual, expected)) {
        fprintf(stderr, "%s: expected '%s', got '%s'\n", guest, expected, actual);
        exit(1);
    }
}
int main(void) {
    _putenv_s("FS_HIDE", "");
    cellfs_set_root_path("D:/Games/Tourline 5 Save Data");
    cellfs_add_path_mapping("/dev_bdvd/PS3_GAME/", "D:/Games/Disc/PS3_GAME/");
    check("/dev_bdvd/PS3_GAME/USRDIR/GT.VOL", "D:\\Games\\Disc\\PS3_GAME\\USRDIR\\GT.VOL");
    check("/dev_bdvd/PS3_GAME", "D:\\Games\\Disc\\PS3_GAME");
    cellfs_add_path_mapping("/dev_bdvd/PS3_GAME/", "E:\\Disc\\PS3_GAME/");
    check("/dev_bdvd/PS3_GAME/USRDIR/GT.VOL", "E:\\Disc\\PS3_GAME\\USRDIR\\GT.VOL");
    cellfs_add_path_mapping("/app_home/", "//server/share/Disc/");
    check("/app_home/USRDIR/a", "\\\\server\\share\\Disc\\USRDIR\\a");
    cellfs_add_path_mapping("PDIPFS/", "/mnt/disc/PDIPFS/");
    check("PDIPFS/K/4D", "\\mnt\\disc\\PDIPFS\\K\\4D");
    check("/dev_hdd0/game/BCUS98114/file", "D:\\Games\\Tourline 5 Save Data\\gamedata\\dev_hdd0\\game\\BCUS98114\\file");
    cellfs_add_path_mapping("/dev_bdvd/PS3_GAME/", "input/pkg/");
    check("/dev_bdvd/PS3_GAME/USRDIR/GT.VOL", "D:\\Games\\Tourline 5 Save Data\\input\\pkg\\USRDIR\\GT.VOL");
    check("/dev_hdd0", "D:\\Games\\Tourline 5 Save Data\\gamedata\\dev_hdd0");
    char tiny[4];
    if (!translate_path("/dev_hdd0/a", tiny, sizeof tiny)) return 2;
    if (!translate_path("/unmapped/a", tiny, sizeof tiny)) return 3;
    cellfs_add_path_mapping("/dev_bdvd/PS3_GAME/", "D:/Disc/");
    _putenv_s("PS3_FS_ABSOLUTE_MAPPINGS", "0");
    check("/dev_bdvd/PS3_GAME/a", "D:\\Games\\Tourline 5 Save Data\\D:\\Disc\\a");
    _putenv_s("PS3_FS_ABSOLUTE_MAPPINGS", "1");
    check("/dev_bdvd/PS3_GAME/a", "D:\\Disc\\a");
    puts("cellFs split-storage path regression checks passed");
}
'''
(folder / 'test.c').write_text(harness)
batch = folder / 'build.cmd'
batch.write_text('@echo off\nset "PATH=C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer;%PATH%"\ncall "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\n'
                 + '"C:\\Program Files\\LLVM\\bin\\clang-cl.exe" /nologo /O2 /MD /D_WIN32 /D_CRT_SECURE_NO_WARNINGS '
                 + f'/Fo"{folder}/" /Fe"{folder}/test.exe" "{folder}/test.c"\n')
subprocess.run(['cmd', '/c', str(batch)], check=True)
env = os.environ.copy()
env.pop('PS3_FS_ABSOLUTE_MAPPINGS', None)
subprocess.run([str(folder / 'test.exe')], env=env, check=True)
