"""Compile production report handling: mapped writes, completion, and rollback."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
runtime = Path(os.environ.get('PS3RECOMP_DIR', root.parent / 'ps3recomp-tourline5'))
folder = Path(tempfile.mkdtemp(prefix='gcm-report-', dir=root / 'scratch'))
source = (runtime / 'libs/video/cellGcmSys.c').read_text()
start = source.index('void cellGcmProcessReportMethod(')
end = source.index('/* Whether IO offsets', start)
harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint32_t u32;
typedef uint64_t u64;
#define CELL_GCM_MAX_REPORT_COUNT 65536
typedef struct { u64 timestamp; u32 value, pad; } CellGcmReportData;
static CellGcmReportData s_report_data[CELL_GCM_MAX_REPORT_COUNT];
static u32 s_report_dma = 0xFEED0000u;
static unsigned char mem[32];
static unsigned writes;
static u64 get_timestamp_ns(void) { return 0x0123456789ABCDEFull; }
static u32 gcm_io2ea(u32 io) {
    return io >= 0x101100 && io < 0x101120 ? 0x40100000u + (io - 0x100000u) : 0;
}
static void vm_write32(u32 ea, u32 value) {
    unsigned off = ea - 0x40101100u;
    assert(off <= 28);
    /* Completion must be published after both payload fields. */
    if (off == 12) assert(writes == 2 && mem[0] == 1 && mem[8] == 0);
    for (unsigned i = 0; i < 4; i++) mem[off + i] = (unsigned char)(value >> (24 - i*8));
    writes++;
}
static void vm_write64(u32 ea, u64 value) {
    assert(ea == 0x40101100u);
    for (unsigned i = 0; i < 8; i++) mem[i] = (unsigned char)(value >> (56 - i*8));
    writes++;
}
'''
harness += source[start:end]
harness += r'''
int main(void) {
    int rollback = getenv("GCM_REPORT_MEMORY") != NULL;
    memset(mem, 0xA5, sizeof mem);
    cellGcmProcessReportMethod(0x1800, 0x01000020); /* legacy host table */
    assert(s_report_data[2].timestamp == get_timestamp_ns());
    assert(s_report_data[2].value == 0xFFFF && writes == 0);
    cellGcmProcessReportMethod(0x1A8, 0xFEED0001);
    cellGcmProcessReportMethod(0x1800, 0x01101100); /* beyond host table */
    if (rollback) { assert(writes == 0 && mem[12] == 0xA5); }
    else {
        const unsigned char expected[16] = {1,0x23,0x45,0x67,0x89,0xAB,0xCD,0xEF,0,0,0xFF,0xFF,0,0,0,0};
        assert(writes == 3 && memcmp(mem, expected, 16) == 0);
        assert(mem[16] == 0xA5); /* no adjacent report overwrite */
    }
    unsigned saved = writes;
    cellGcmProcessReportMethod(0x1800, 0x01101101); /* unaligned */
    cellGcmProcessReportMethod(0x1800, 0x01102100); /* unmapped */
    cellGcmProcessReportMethod(0x1A8, 0xDEADBEEF); /* unsupported context */
    cellGcmProcessReportMethod(0x1800, 0x01101100);
    cellGcmProcessReportMethod(0x1A8, 0xFEED0000); /* local remains host table */
    cellGcmProcessReportMethod(0x1800, 0x01101100);
    assert(writes == saved);
    puts(rollback ? "PASS report memory rollback" : "PASS mapped report payload, big endian, completion last, bounds and legacy table");
}
'''
(folder / 'test.c').write_text(harness)
batch = folder / 'build.cmd'
batch.write_text('@echo off\nset "PATH=C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer;%PATH%"\n'
    'call "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\n'
    'if errorlevel 1 exit /b 1\n'
    '"C:\\Program Files\\LLVM\\bin\\clang-cl.exe" /nologo /O2 /MD /std:c17 /D_CRT_SECURE_NO_WARNINGS '
    f'/Fo"{folder}/test.obj" /Fe"{folder}/test.exe" "{folder}/test.c"\n')
subprocess.run(['cmd', '/c', str(batch)], check=True)
for mode in [None, '0']:
    env = os.environ.copy()
    for key in ['GCM_REPORT_MEMORY', 'GCM_REPORT_FULL']:
        env.pop(key, None)
    if mode is not None:
        env['GCM_REPORT_MEMORY'] = mode
    subprocess.run([str(folder / 'test.exe')], env=env, check=True)
