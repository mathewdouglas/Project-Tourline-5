"""Compile/test the real Windows page tracker and vertex-cache code; no game/GPU.

PS3RECOMP_DIR selects the runtime. Uses one compiler process and small synthetic
buffers, so this does not launch/stop any game or change build outputs.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
RUNTIME = Path(os.environ.get("PS3RECOMP_DIR", ROOT.parent / "ps3recomp-tourline5")).resolve()


def main():
    source = (RUNTIME / "libs/video/rsx_d3d12_backend.c").read_text(encoding="utf-8")
    def between(start, end):
        return source[source.index(start):source.index(end, source.index(start))]
    def decl(pattern):
        found = re.search(pattern, source)
        assert found, pattern
        return found.group(0)
    parts = [decl(r"typedef struct \{ u32 S, loc, lo, hi; \} GfStream;"),
             decl(r"typedef struct \{ u32 dst, src_lo, base, region; int reused, cached; \} GfPlace;"),
             decl(r"typedef struct \{ u64 key;[\s\S]+?\} VcEnt;"),
             "static VcEnt* s_vc;\n",
             between("static int vraw_copy(", "/* Guest bytes into ring offset"),
             between("static VcEnt* vc_get(", "static void vc_frame_end(void)\n{"),
             between("static int vc_place(", "/* The per-frame ring for one stream:")]
    folder = Path(tempfile.mkdtemp(prefix="vertex-watch-", dir=ROOT / "scratch"))
    (folder / "vertex_watch_impl.inc").write_text("\n".join(parts), encoding="utf-8")
    vm = (RUNTIME / "runtime/memory/vm.h").read_text(encoding="utf-8")
    start = vm.index("    for (int attempt = 0; attempt < 8 && !vm_base;")
    end = vm.index("    if (!vm_base) return CELL_ENOMEM;", start)
    (folder / "vertex_watch_reserve.inc").write_text(vm[start:end], encoding="utf-8")
    vswhere = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
    vs = subprocess.check_output([str(vswhere), "-latest", "-products", "*", "-property", "installationPath"], text=True).strip()
    assert vs, "Visual Studio installation not found"
    batch = folder / "build.cmd"
    batch.write_text('@echo off\nset "PATH=' + str(vswhere.parent) + ';%PATH%"\ncall "' + vs + '/VC/Auxiliary/Build/vcvars64.bat" >nul\n'
        + '"C:/Program Files/LLVM/bin/clang-cl.exe" /nologo /O2 /MD /W4 /D_CRT_SECURE_NO_WARNINGS '
        + f'/I"{RUNTIME}/libs/video" /I"{folder}" /Fo"{folder}/" /Fe"{folder}/test.exe" '
        + f'"{ROOT}/scripts/test/vertex_write_watch.c"\n', encoding="utf-8")
    subprocess.run(["cmd", "/c", str(batch)], check=True)
    for mode, strict in (("0", False), ("1", False), ("2", False), ("1", True)):
        env = os.environ.copy()
        env.pop("RSX_VTXCACHE_VERIFY", None)
        env.pop("PS3_NO_WRITE_WATCH", None)
        env["RSX_VTXCACHE_WRITE_WATCH"] = mode
        if strict:
            env["RSX_VTXCACHE_VERIFY"] = "draw"
        subprocess.run([str(folder / "test.exe"), mode], env=env, check=True, timeout=15)
    for mode, no_watch in (("1", "1"), ("3", "")):
        env = os.environ.copy()
        env["RSX_VTXCACHE_WRITE_WATCH"] = mode
        env.pop("PS3_NO_WRITE_WATCH", None)
        if no_watch:
            env["PS3_NO_WRITE_WATCH"] = no_watch
        subprocess.run([str(folder / "test.exe"), mode, "reserve-only"], env=env, check=True, timeout=15)
    print("Vertex write-watch rollback, enabled and byte-verified modes passed")


if __name__ == "__main__":
    main()
