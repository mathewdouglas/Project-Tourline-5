#!/usr/bin/env python3
"""Keep the SPU registers of hot lifted functions in C locals (GT5 Phase 44).

Lifted SPU code keeps every register in ctx->gpr[], in memory. Each local-
store write goes through a byte pointer, which may alias ctx, so the compiler
reloads and re-stores the registers around it: the hot render-body loops of
GT5 (body 0x0196FF40, blocks 0xB240, 0xAD30, ...; 59% of workload 4's lane
time) are mostly loads and stores of ctx->gpr.

For each function listed under "spu_regpromo" in config/targets.json
([image directory, [block address, ...]]) this adds, in the lifted source:

  <fn>_rp   the same statements, in the same order, with every ctx->gpr[N]
            replaced by a local rN (loaded at entry). Before every exit, and
            before any call that is given ctx (other than spu_ls_read128 /
            spu_ls_write128), the locals the function writes are stored back
            to ctx->gpr; after such a call all of them are reloaded. An exit
            line itself still reads ctx->gpr, after the store.
  <fn>_oc,  check variants: the original and the promoted body with every
  <fn>_rpc  exit turned into a plain return (ctx->pc and ctx->int_enable set
            as the exit would; no tail call, no trampoline). Only made for
            functions whose only calls given ctx are their exits.

and as the first statement of <fn> itself a switch on GT5P_SPU_REGPROMO
(src/gt5p_spu_native.c): 0 the original code (default until measured);
1 a tail call into <fn>_rp; 2 check: run <fn>_oc and <fn>_rpc from the same
state (registers, pc, interrupt enable and the whole local store), compare,
put the state back and run the original; 3 A/B within one boot: the promoted
copy while g_spu_exp is set (SPU_EXP_AB=1 SPURS_LANE_CYC=1 flips it every
lane-timing window; scripts/diag/abcyc.py --key exp). The promoted copy is only used when
the lifted code is built without the local-store probes (SPU_LS_NO_PROBES,
the default), since the probes read ctx->gpr[0].

Run after every SPU lift or --chain, after spu_native_patch.py; generated/ is
not in the repo.

    python scripts/spu_regpromo.py            # patch (idempotent)
    python scripts/spu_regpromo.py --check    # report, change nothing
    python scripts/spu_regpromo.py --restore  # undo
"""
import re
import sys
from pathlib import Path

import target

GEN = Path(__file__).resolve().parent.parent / "generated" / "spu_lifted"
MARK = "/* spu_regpromo */"
BEGIN = "/* spu_regpromo begin */"
END = "/* spu_regpromo end */"

GPR = re.compile(r"ctx->gpr\[(\d+)\]")
GPR_WRITE = re.compile(r"ctx->gpr\[(\d+)\](?:\._\w+\[\d+\])?\s*(?:[-+*/|&^]|<<|>>)?=(?!=)")
CTX_CALL = re.compile(r"\b([A-Za-z_]\w*)\s*\(\s*ctx\s*[,)]")
LS_HELPERS = {"spu_ls_read128", "spu_ls_write128"}
TAIL = re.compile(r"__attribute__\(\(musttail\)\)\s+return\s+[A-Za-z_]\w*\(ctx\);")
PLAIN_TAIL = re.compile(r"\breturn\s+[A-Za-z_]\w*\(ctx\);")
TRAMP = re.compile(r"g_spu_trampoline_fn\s*=\s*[A-Za-z_]\w*;")
RET = re.compile(r"SPU_RET\(ctx\);")
RET_REG = re.compile(r"SPU_RET_REG\(ctx,\s*(\d+)\);")

PROLOGUE = [
    f"extern volatile int g_gt5p_spu_regpromo; {MARK}",
    f"int gt5p_spu_regpromo_init(void); {MARK}",
    f"void gt5p_spu_regpromo_check(spu_context* ctx, void (*orig)(spu_context*), "
    f"void (*promo)(spu_context*), uint32_t block); {MARK}",
    f"#ifdef SPU_LS_NO_PROBES {MARK}",
    f"#define GT5P_RP_MODE() (g_gt5p_spu_regpromo >= 0 ? g_gt5p_spu_regpromo : gt5p_spu_regpromo_init()) {MARK}",
    f"#else {MARK}",
    f"#define GT5P_RP_MODE() 0 {MARK}",
    f"#endif {MARK}",
]


def is_exit(line):
    return "return" in line or "SPU_RET" in line


def other_calls(line):
    return [m.group(1) for m in CTX_CALL.finditer(line) if m.group(1) not in LS_HELPERS]


def split_if_exit(line):
    """`<ind>if (COND) { BODY }` with balanced parentheses -> (ind, COND, BODY)."""
    m = re.match(r"^(\s*)if \(", line)
    if not m or not line.rstrip().endswith("}"):
        return None
    i, depth = m.end(), 1
    while i < len(line) and depth:
        depth += {"(": 1, ")": -1}.get(line[i], 0)
        i += 1
    if depth or not line[i:].startswith(" { "):
        return None
    cond, body = line[m.end():i - 1], line[i + 3:].rstrip()[:-1].rstrip()
    if other_calls(cond) or "return" in cond:
        return None
    return m.group(1), cond, body


def stopify(line):
    """An exit that sets ctx->pc / ctx->int_enable as before and then returns."""
    line = TAIL.sub("return;", line)
    line = PLAIN_TAIL.sub("return;", line)
    line = TRAMP.sub("(void)0;", line)
    line = RET.sub("{ ctx->pc = ctx->gpr[0]._u32[0]; return; }", line)
    line = RET_REG.sub(lambda m: "{ ctx->pc = ctx->gpr[%s]._u32[0]; return; }" % m.group(1), line)
    return line


def promote(name, body, regs, written):
    """body: lines between the header and the closing brace."""
    spill = " ".join(f"ctx->gpr[{n}] = r{n};" for n in written)
    reload = " ".join(f"r{n} = ctx->gpr[{n}];" for n in regs)
    out = [f"        u128 r{n} = ctx->gpr[{n}];" for n in regs]
    for line in body:
        if is_exit(line):
            split = split_if_exit(line)
            if split:
                ind, cond, rest = split
                cond = GPR.sub(r"r\1", cond)
                out.append(f"{ind}if ({cond}) {{ {spill} {rest} }}")
            else:
                ind = re.match(r"\s*", line).group(0)
                out.append(f"{ind}{{ {spill} }}")
                out.append(line)
        elif other_calls(line):
            ind = re.match(r"\s*", line).group(0)
            out.append(f"{ind}{{ {spill} }}")
            out.append(line)
            out.append(f"{ind}{{ {reload} }}")
        else:
            out.append(GPR.sub(r"r\1", line))
    return out


def function_body(lines, image, block):
    head = f"void gt5p_{image}_spu_func_{block:08X}(spu_context* ctx) {{"
    try:
        start = lines.index(head)
    except ValueError:
        return None, None, "not found"
    try:
        end = lines.index("}", start + 1)
    except ValueError:
        return None, None, "no closing brace"
    return start, end, None


def check_shape(body):
    for line in body:
        s = line.strip()
        if s and not (s.endswith(";") or s.endswith("}") or s.endswith(":") or s.endswith("*/")):
            return f"multi-line statement: {s[:60]}"
        if "&ctx->gpr" in line:
            return "takes the address of a register"
        if re.search(r"\br\d+\b", GPR.sub("", line)):
            return "already uses a name rN"
    return None


def patch_image(image, blocks, text):
    lines = text.split("\n")
    lines = strip(lines)
    report = []
    adds = {}          # line index of the function header -> (redirect, new functions)
    for block in blocks:
        start, end, err = function_body(lines, image, block)
        if err:
            report.append(f"0x{block:X}: {err}")
            continue
        body = lines[start + 1:end]
        err = check_shape(body)
        if err:
            report.append(f"0x{block:X}: skipped, {err}")
            continue
        regs = sorted({int(n) for l in body for n in GPR.findall(l)})
        written = sorted({int(n) for l in body for n in GPR_WRITE.findall(l)})
        calls = sorted({c for l in body if not is_exit(l) for c in other_calls(l)})
        name = f"gt5p_{image}_spu_func_{block:08X}"
        new = [BEGIN, f"static void {name}_rp(spu_context* ctx) {{"]
        new += promote(name, body, regs, written) + ["}"]
        checkable = not calls
        if checkable:
            stopped = [stopify(l) for l in body]
            if any(other_calls(l) for l in stopped):
                checkable = False
        if checkable:
            new += [f"static void {name}_oc(spu_context* ctx) {{"] + stopped + ["}"]
            new += [f"static void {name}_rpc(spu_context* ctx) {{"]
            new += promote(name, stopped, regs, written) + ["}"]
        new.append(END)
        chk = (f" if (_rpm == 2) gt5p_spu_regpromo_check(ctx, {name}_oc, {name}_rpc, 0x{block:X}u);"
               if checkable else "")
        redirect = (f"        {{ const int _rpm = GT5P_RP_MODE(); if (_rpm == 1 || (_rpm == 3 && g_spu_exp)) {{ "
                    f"__attribute__((musttail)) return {name}_rp(ctx); }}{chk} }} {MARK}")
        adds[start] = (redirect, new)
        report.append(f"0x{block:X}: {len(regs)} registers ({len(written)} written)"
                      + ("" if checkable else f", no check variant (calls {', '.join(calls) or 'in exits'})")
                      + (f", spills around {', '.join(calls)}" if calls else ""))
    out = []
    first = min(adds) if adds else None
    for i, line in enumerate(lines):
        if i == first:
            out.extend(PROLOGUE)
        if i in adds:
            out.extend(adds[i][1])
        out.append(line)
        if i in adds:
            out.append(adds[i][0])
    return "\n".join(out), report


def strip(lines):
    out, skip = [], False
    for l in lines:
        if l == BEGIN:
            skip = True
            continue
        if l == END:
            skip = False
            continue
        if skip or MARK in l:
            continue
        out.append(l)
    return out


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else ""
    want = {image: [int(b, 16) for b in blocks] for image, blocks in target.TARGET.get("spu_regpromo", [])}
    bad = 0
    for image in sorted(set(want) | {d.name for d in GEN.glob("*") if d.is_dir()}):
        src = GEN / image / "spu_recomp.c"
        if not src.exists():
            if image in want:
                print(f"  {image}: no lifted source")
                bad += 1
            continue
        text = src.read_text(encoding="utf-8")
        if mode == "--restore" or image not in want:
            clean = "\n".join(strip(text.split("\n")))
            if clean != text:
                if mode == "--check":
                    print(f"  {image}: carries promoted copies but is not listed")
                    continue
                src.write_text(clean, encoding="utf-8", newline="\n")
                print(f"  {image}: restored")
            continue
        new, report = patch_image(image, want[image], text)
        for r in report:
            print(f"  {image} {r}")
        if any("not found" in r or "skipped" in r for r in report):
            bad += 1
        if mode == "--check":
            print(f"  {image}: {'patched' if new == text else 'needs patching'}")
            continue
        if new != text:
            src.write_text(new, encoding="utf-8", newline="\n")
        print(f"  {image}: {len(want[image])} functions")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
