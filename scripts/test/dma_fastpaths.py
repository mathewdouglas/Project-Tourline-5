"""Check DMA fast paths in a private runtime, at idle priority; never run the game.

Default: materialize the pinned baseline plus the shipped compatibility patch.
--patch can check a candidate cumulative patch before publishing it.
--baseline-header also compares with a pre-change production DMA header.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
IDLE = subprocess.IDLE_PRIORITY_CLASS if os.name == "nt" else 0


def run(command, **kwargs):
    return subprocess.run(command, check=True, creationflags=IDLE, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--patch", type=Path)
    parser.add_argument("--baseline-header", type=Path)
    args = parser.parse_args()
    manifest = json.loads((ROOT / "config/ps3recomp.json").read_text())
    runtime = Path(os.environ.get("PS3RECOMP_DIR", ROOT.parent / manifest["directory"]))
    (ROOT / "scratch").mkdir(exist_ok=True)
    folder = Path(tempfile.mkdtemp(prefix="dma-fast-test-", dir=ROOT / "scratch"))
    stage = folder / "runtime"
    stage.mkdir()
    env = dict(os.environ, GIT_INDEX_FILE=str(folder / "runtime.index"))
    git = ["git", "-C", str(runtime)]
    run(git + ["read-tree", manifest["commit"]], env=env)
    run(git + ["-c", "core.autocrlf=true", "checkout-index", "--all",
               f"--prefix={stage.as_posix()}/"], env=env)
    patch = (args.patch or ROOT / manifest["patch"]).resolve()
    run(["git", "apply", "--unsafe-paths", "--whitespace=nowarn",
         f"--directory={stage.as_posix()}", str(patch)], cwd=ROOT)
    header = stage / "runtime/spu/spu_dma.h"
    source = header.read_text(encoding="utf-8")
    channels = (stage / "runtime/spu/spu_channels.c").read_text(encoding="utf-8")
    assert "spu_dma_fast() && mfc_context_write(ctx, channel, v)" in channels
    assert "mfc_channel_read(spu_dma_fast() ? NULL : mfc_for(ctx), ctx, channel)" in channels
    gate = source[source.index("static inline int spu_dma_diag"):source.index("static inline int mfc_context_write")]
    names = re.findall(r'"(SPU_[A-Z0-9_]+)"', gate)
    names = sorted(set(names))
    assert len(names) == 27, names
    # Detect accidental behavior edits to the header while adapting the gates.
    if args.baseline_header:
        original = args.baseline_header.read_text(encoding="utf-8")
        reverted = source[:source.index("/* DMA/channel fast paths,")] + source[source.index("static inline int mfc_do_transfer"):]
        reverted = reverted.replace("if (spu_dma_diag()) ", "")
        reverted = reverted.replace("if (spu_dma_diag() && ", "if (")
        assert reverted == original, "Header changes extend beyond helpers and diagnostic guards"
        (header.parent / "spu_dma_before.h").write_bytes(args.baseline_header.read_bytes())

    includes = " ".join(f'/I"{stage / p}"' for p in ("include", "runtime/spu", "runtime/ppu", "runtime/memory"))
    cc = '"C:\\Program Files\\LLVM\\bin\\clang-cl.exe" /nologo /O2 /MD /arch:AVX2 /D_CRT_SECURE_NO_WARNINGS '
    lines = ['@echo off', 'set "PATH=C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer;%PATH%"',
             'call "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul',
             'if errorlevel 1 exit /b 1']
    for label, defines in [("fast", "/DDMA_FAST_TEST")]+(
            [("before", '/DDMA_HEADER=\\"spu_dma_before.h\\"')] if args.baseline_header else []):
        lines += [cc + includes + f' {defines} /Fo"{folder}/{label}.obj" /Fe"{folder}/{label}.exe" "{ROOT}/scripts/test/dma_fastpaths.c"',
                  'if errorlevel 1 exit /b 1']
    # Compile the entire real channel implementation, not a test copy.
    lines += [cc + includes + f' /c /Fo"{folder}/spu_channels.obj" "{stage}/runtime/spu/spu_channels.c"',
              'if errorlevel 1 exit /b 1']
    batch = folder / "build.cmd"
    batch.write_text("\n".join(lines) + "\n", encoding="ascii")
    run(["cmd", "/c", str(batch)])
    clean = dict(os.environ)
    for key in list(clean):
        if key.startswith(("SPU_", "MFC_", "YDKJ_", "LBP_")):
            clean.pop(key)

    def execute(exe="fast", extra=None, gate_only=False):
        command = [str(folder / f"{exe}.exe")] + (["gate"] if gate_only else [])
        result = run(command, env=dict(clean, **(extra or {})), capture_output=True, text=True, timeout=15)
        return result.stdout, result.stderr

    assert execute(gate_only=True)[0].strip() == "fast=1 diag=0"
    assert execute(extra={"SPU_DMA_FAST": "0"}, gate_only=True)[0].strip() == "fast=0 diag=1"
    for name in names:
        for value in ("0", "1"):
            assert execute(extra={name: value}, gate_only=True)[0].strip() == "fast=1 diag=1", name

    reference = execute("before") if args.baseline_header else execute(extra={"SPU_DMA_FAST": "0"})
    for extra in ({}, {"SPU_DMA_FAST": "0"}, {"SPU_DMA_DIAG": "1"}):
        assert execute(extra=extra) == reference, extra
    # Selected probes must produce the same diagnostics, not just the same DMA bytes.
    probes = ({"SPU_DMA_RANGE": "10000-18000"}, {"SPU_DUMP_GET": "0x10000"},
              {"SPU_GETHIST": "1", "SPU_PUTHIST": "1"},
              {"SPU_TAGPROBE": "1", "SPU_CMDHIST": "1"},
              {"SPU_PUTVAL": "0,10000,18000"})
    for extra in probes:
        old = execute("before", extra) if args.baseline_header else execute(extra=dict(extra, SPU_DMA_FAST="0"))
        assert execute(extra=extra) == old, extra
    print(reference[0].strip())
    print(f"DMA state/diagnostic parity passed; {len(names)} probe gates; production channels compiled")
    print(f"Private artifacts: {folder}")


if __name__ == "__main__":
    main()
