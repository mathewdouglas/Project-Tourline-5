"""Register promotion (scripts/spu_regpromo.py) against the lifted code it copies.

For every function listed under "spu_regpromo" for the target, the private
lifted source (generated/spu_lifted/<image>/spu_recomp.c, without any patch)
is turned into the two check variants the game's GT5P_SPU_REGPROMO=2 uses: the
original and the promoted body, each stopping at its first exit. A loop guard
is added to both in the same way (a local count of taken back edges; after
200 it leaves through one more exit, pc 0x3FFFC), so random states always end. Each pair runs
from 100 random register/local-store states; registers, pc, interrupt enable
and the whole local store must agree (lanes that are NaN in both count as
equal: operand order of a commutative float op may pick a different NaN, as
any rebuild may). The production check routine (src/gt5p_spu_native.c) is also
run and must leave the state untouched and count no difference.

Needs Visual Studio 2022 and LLVM (clang-cl), like the other scripts/test
builds. Extracted lifted code stays in an ignored scratch folder.

    python scripts/test/spu_regpromo.py
"""
from pathlib import Path
import os, re, subprocess, sys, tempfile

root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / "scripts"))
import spu_regpromo as rp  # noqa: E402
import target  # noqa: E402

runtime = Path(os.environ.get("PS3RECOMP_DIR", root.parent / "ps3recomp-tourline5")).resolve()
folder = Path(tempfile.mkdtemp(prefix="spu-regpromo-", dir=root / "scratch"))
GOTO = re.compile(r"^(\s*)if \((.*)\) goto (loc_[0-9A-F]+);$")
PLAIN_GOTO = re.compile(r"^(\s*)goto (loc_[0-9A-F]+);$")

out, pairs = [], []
for image, blocks in target.TARGET.get("spu_regpromo", []):
    src = root / "generated" / "spu_lifted" / image / "spu_recomp.c"
    lines = rp.strip(src.read_text(encoding="utf-8").split("\n"))
    for b in blocks:
        block = int(b, 16)
        start, end, err = rp.function_body(lines, image, block)
        if err:
            sys.exit(f"{image} 0x{block:X}: {err}")
        body = ["        int _rp_guard = 0;"]
        for l in lines[start + 1:end]:
            m = GOTO.match(l)
            if m:
                l = f"{m.group(1)}if ({m.group(2)}) {{ if (++_rp_guard > 200) goto rp_guard_exit; goto {m.group(3)}; }}"
            m = PLAIN_GOTO.match(l)
            if m:
                l = f"{m.group(1)}{{ if (++_rp_guard > 200) goto rp_guard_exit; goto {m.group(2)}; }}"
            body.append(l)
        body += ["rp_guard_exit:", "        { ctx->pc = 0x3FFFCu; return; }"]
        regs = sorted({int(n) for l in body for n in rp.GPR.findall(l)})
        written = sorted({int(n) for l in body for n in rp.GPR_WRITE.findall(l)})
        stopped = [rp.stopify(l) for l in body]
        if any(rp.other_calls(l) for l in stopped):
            sys.exit(f"{image} 0x{block:X}: calls other than its exits")
        name = f"t_{image}_{block:05X}"
        out += [f"static void {name}_oc(spu_context* ctx) {{"] + stopped + ["}"]
        out += [f"static void {name}_rpc(spu_context* ctx) {{"] + rp.promote(name, stopped, regs, written) + ["}"]
        pairs.append(f'    {{ {name}_oc, {name}_rpc, 0x{block:X}u, "{image}" }},')

(folder / "pairs.inc").write_text("\n".join(out) + "\n#define RP_PAIRS \\\n"
                                  + " \\\n".join(p for p in pairs) + "\n", encoding="utf-8")
batch = folder / "build.cmd"
batch.write_text('@echo off\nset "PATH=C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer;%PATH%"\n'
                 + 'call "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\n'
                 + '"C:\\Program Files\\LLVM\\bin\\clang-cl.exe" /nologo /O2 /MD /D_CRT_SECURE_NO_WARNINGS /DSPU_LS_NO_PROBES -msse4.1 '
                 + f'/I"{runtime}/runtime/spu" /I"{folder}" /Fo"{folder}/" /Fe"{folder}/test.exe" "{root}/scripts/test/spu_regpromo.c"\n')
subprocess.run(["cmd", "/c", str(batch)], check=True)
subprocess.run([str(folder / "test.exe")], check=True, timeout=600)
