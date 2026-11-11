"""Merge a loop of lifted SPU functions into one C function with the SPU
registers in locals (experiment, GT5 Phase 45; used by
scripts/test/spu_loop_bench.py).

A hot SPU loop is lifted as several small functions that tail-call each
other; each keeps the SPU registers in ctx->gpr, so every operation loads
and stores memory, and register promotion of one function at a time
(scripts/spu_regpromo.py) reloads and spills its registers on every call.
Here the members become labelled blocks of one function: tail calls between
members are gotos, registers are locals loaded once on entry, and the written
ones are stored back before anything that leaves the region (a tail call
elsewhere, a trampoline return, SPU_RET, any other return). Each member's own
function starts with a redirect into the region, so every way in is covered.

region_text(image, members, text) -> (new text, report)
"""
import re

GPR = re.compile(r"ctx->gpr\[(\d+)\]")
GPR_WRITE = re.compile(r"ctx->gpr\[(\d+)\](?:\._\w+\[\d+\])?\s*(?:[-+*/|&^]|<<|>>)?=(?!=)")
LABEL = re.compile(r"\bloc_([0-9A-F]+)\b")


def region_text(image, members, text):
    lines = text.split("\n")
    nl_cr = lines and lines[0].endswith("\r")
    lines = [l.rstrip("\r") for l in lines]
    fname = lambda a: f"gt5p_{image}_spu_func_{a:08X}"
    region = f"gt5p_{image}_region_{members[0]:08X}"
    bodies, heads = {}, {}
    for m in members:
        head = f"void {fname(m)}(spu_context* ctx) {{"
        s = lines.index(head)
        e = lines.index("}", s + 1)
        heads[m] = s
        bodies[m] = lines[s + 1:e]
    allb = [l for m in members for l in bodies[m]]
    if any("&ctx->gpr" in l for l in allb):
        raise SystemExit("a member takes the address of a register")
    regs = sorted({int(n) for l in allb for n in GPR.findall(l)})
    written = sorted({int(n) for l in allb for n in GPR_WRITE.findall(l)})
    spill = " ".join(f"ctx->gpr[{n}] = r{n};" for n in written) + " "
    tail = re.compile(r"__attribute__\(\(musttail\)\) return (gt5p_%s_spu_func_([0-9A-F]{8}))\(ctx\);" % image)
    out = [f"static void {region}(spu_context* ctx) {{"]
    out += [f"        u128 r{n} = ctx->gpr[{n}];" for n in regs]
    out.append("        switch (ctx->pc & 0x3FFFCu) {")
    out += [f"        case 0x{m:X}u: goto R_{m:X};" for m in members]
    out.append(f"        default: {{ {spill}return; }}")
    out.append("        }")
    for m in members:
        out.append(f"R_{m:X}:;")
        for l in bodies[m]:
            # The lifter braces every if body, so statements put in front of a
            # leaving statement stay on its path. Refuse anything else.
            if re.search(r"(?<!musttail\))\)\s*(return|SPU_RET|__attribute__)", l) or re.search(r"\belse\s+(return|SPU_RET)", l):
                raise SystemExit(f"unbraced leaving statement in 0x{m:X}: {l.strip()[:80]}")
            l = GPR.sub(r"r\1", l)
            l = LABEL.sub(lambda x: f"loc_{x.group(1)}_{m:X}", l)
            # Tail calls to members become gotos; every other way out first
            # stores the written registers back.
            l = tail.sub(lambda x: (f"goto R_{int(x.group(2), 16):X};" if int(x.group(2), 16) in members
                                    else f"@@SPILL@@__attribute__((musttail)) @@RET@@ {x.group(1)}(ctx);"), l)
            l = re.sub(r"\breturn\b", "@@SPILL@@return", l)
            l = l.replace("@@RET@@", "return")
            l = re.sub(r"\bSPU_RET(_REG)?\(", r"@@SPILL@@SPU_RET\1(", l)
            out.append(l.replace("@@SPILL@@", spill))
    out.append("}")
    # Redirect each member's own function into the region.
    res = []
    for i, l in enumerate(lines):
        if i == min(heads.values()):
            res.extend(out)
        res.append(l)
        for m, s in heads.items():
            if i == s:
                res.append(f"        {{ ctx->pc = 0x{m:X}u; __attribute__((musttail)) return {region}(ctx); }}")
    text = "\n".join(res)
    if nl_cr:
        text = text.replace("\n", "\r\n")
    return text, [f"region {region}: {len(members)} functions, {len(regs)} registers ({len(written)} written)"]
