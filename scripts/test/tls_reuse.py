"""Test PPU TLS block recycling (issue #74):
GT5 churns short-lived PPU threads across long sessions (714 creates / 666 exits
in #74). Without recycling, the 1 MB TLS window fills at 248 thread creations
and every subsequent thread shares the main thread's TLS block.

PS3_TLS_REUSE=1 (default) must sustain 1000+ alloc/free cycles with zero
fallbacks, re-initialized template bytes on reuse, and double-free / main-TP
safety. PS3_TLS_REUSE=0 must reproduce the old bump-only exhaustion."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
runtime = Path(os.environ.get('PS3RECOMP_DIR', root.parent / 'ps3recomp-tourline5'))
(root / 'scratch').mkdir(exist_ok=True)
folder = Path(tempfile.mkdtemp(prefix='tls-reuse-', dir=root / 'scratch'))

harness = folder / 'tls_reuse_test.cpp'
harness.write_text(r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

extern "C" {
uint8_t* vm_base = nullptr;
void (*g_ppu_thread_entry_trampoline)(void*) = nullptr;
void (*g_ppu_tls_free_fn)(uint32_t tp) = nullptr;
void ppu_tls_set_template(uint32_t vaddr, uint32_t filesz, uint32_t memsz);
uint32_t ppu_tls_new_block(void);
void ppu_tls_free_block(uint32_t tp);
void ppu_install_thread_trampoline(void);
}

int main(int argc, char** argv)
{
    const int expect_reuse = (argc > 1 && argv[1][0] == '1') ? 1 : 0;

    /* Reserve 0x11000000 bytes (272 MB) of sparse virtual memory so
     * PPU_TLS_IMG (0x10F00000..0x11000000) and template at 0x0176CEE0 are valid. */
#ifdef _WIN32
    vm_base = (uint8_t*)VirtualAlloc(nullptr, 0x11000000u, MEM_RESERVE, PAGE_NOACCESS);
    assert(vm_base != nullptr);
    assert(VirtualAlloc(vm_base + 0x0176C000u, 0x2000u, MEM_COMMIT, PAGE_READWRITE) != nullptr);
    assert(VirtualAlloc(vm_base + 0x10F00000u, 0x00100000u, MEM_COMMIT, PAGE_READWRITE) != nullptr);
#else
    return 1;
#endif

    /* Populate a GT5-like TLS template: filesz=8, memsz=0x1FC */
    const uint32_t tpl_va = 0x0176CEE0u;
    const uint8_t tpl_bytes[8] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x12, 0x34, 0x56, 0x78 };
    memcpy(vm_base + tpl_va, tpl_bytes, 8);
    ppu_tls_set_template(tpl_va, 8, 0x1FC);
    ppu_install_thread_trampoline();
    assert(g_ppu_tls_free_fn == ppu_tls_free_block);

    if (expect_reuse) {
        /* 1. 48 concurrent threads, churned over 1,200 create/exit cycles (> 248 limit). */
        std::vector<uint32_t> live;
        std::set<uint32_t> live_set;
        for (int round = 0; round < 1200; round++) {
            if (live.size() >= 48) {
                uint32_t victim = live.front();
                live.erase(live.begin());
                live_set.erase(victim);
                /* Dirty the block before freeing to verify re-init on reuse. */
                memset(vm_base + (victim - 0x7000u), 0xAA, 0x1FC);
                ppu_tls_free_block(victim);
                /* Double-free must be a safe no-op. */
                ppu_tls_free_block(victim);
            }
            uint32_t tp = ppu_tls_new_block();
            assert(tp != 0);
            assert(tp != 0x10F07000u); /* never main thread's PPU_TLS_TP */
            assert(live_set.insert(tp).second); /* distinct from all live threads */
            uint32_t base = tp - 0x7000u;
            assert(memcmp(vm_base + base, tpl_bytes, 8) == 0);
            for (uint32_t i = 8; i < 0x1FC; i++) assert(vm_base[base + i] == 0);
            live.push_back(tp);
        }
        /* Main TP and out-of-range frees must be ignored safely. */
        ppu_tls_free_block(0);
        ppu_tls_free_block(0x10F07000u);
        ppu_tls_free_block(0x12345678u);
        for (uint32_t tp : live) ppu_tls_free_block(tp);
        printf("tls_reuse=1: 1200 churn cycles (48 concurrent) OK\n");
    } else {
        /* Rollback mode: bump-only must exhaust around 248 allocations. */
        int ok = 0;
        for (int i = 0; i < 400; i++) {
            uint32_t tp = ppu_tls_new_block();
            if (!tp) break;
            ppu_tls_free_block(tp);
            ok++;
        }
        assert(ok == 248);
        printf("tls_reuse=0: exhausted at %d slots as expected\n", ok);
    }
    return 0;
}
''', encoding='ascii')

(folder / 'ppu_recomp.h').write_text(
    '#pragma once\n#include "ppu_context.h"\n'
    'typedef struct { uint64_t addr; void (*func)(ppu_context*); const char* name; } func_entry;\n'
    'extern "C" { extern const func_entry function_table[]; extern const uint64_t function_table_count; }\n'
    'extern "C" uint8_t vm_read8(uint64_t a);\n'
    'extern "C" uint16_t vm_read16(uint64_t a);\n'
    'extern "C" uint32_t vm_read32(uint64_t a);\n'
    'extern "C" uint64_t vm_read64(uint64_t a);\n'
    'extern "C" void vm_write8(uint64_t a, uint8_t v);\n'
    'extern "C" void vm_write16(uint64_t a, uint16_t v);\n'
    'extern "C" void vm_write32(uint64_t a, uint32_t v);\n'
    'extern "C" void vm_write64(uint64_t a, uint64_t v);\n',
    encoding='ascii')

src = runtime / 'runtime' / 'ppu' / 'ppu_loader.cpp'
batch = folder / 'build.cmd'
batch.write_text(
    '@echo off\ncall "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\n'
    '"C:\\Program Files\\LLVM\\bin\\clang-cl.exe" /nologo /O2 /MD /EHsc /std:c++17 /D_CRT_SECURE_NO_WARNINGS '
    '-Wno-everything '
    f'/I"{folder}" /I"{runtime}/runtime/ppu" /I"{runtime}/runtime" /I"{runtime}/runtime/memory" /I"{runtime}/runtime/syscalls" /I"{runtime}/include" '
    f'/Fo"{folder}/" /Fe"{folder}/test.exe" '
    f'"{src}" "{harness}" /link /FORCE:UNRESOLVED\n',
    encoding='ascii')
subprocess.run(['cmd', '/c', str(batch)], check=True, cwd=root)
for mode in ('1', '0'):
    env = os.environ.copy()
    env['PS3_TLS_REUSE'] = mode
    env['PS3_TLS_PACK'] = '1'
    subprocess.run([str(folder / 'test.exe'), mode], env=env, check=True, timeout=60)
print('tls_reuse: all assertions passed')
