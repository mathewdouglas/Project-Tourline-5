#!/usr/bin/env python3
"""Merge a hot loop of lifted SPU functions into one C function (GT5 Phase 45).

A hot SPU loop is lifted as several small functions that tail-call each
other, with every SPU register in ctx->gpr[] in memory. For each region
listed under "spu_regions" in config/targets.json ([image directory,
[function address, ...]]) this adds, in the lifted source, one static
function holding all of them:

  - each member is a labelled block; a tail call from one member to another
    is a goto (the back edges' interrupt/trampoline checks are kept);
  - the SPU registers the members use are __m128i locals, loaded from
    ctx->gpr on entry; each helper still receives and returns its u128
    through a no-op conversion, so the arithmetic is the same code;
  - before anything that leaves the region (a tail call to a non-member, a
    trampoline return, SPU_RET, any other return) the registers the members
    write are stored back to ctx->gpr.

Each member's own function then starts with a switch on GT5P_SPU_REGION
(src/gt5p_spu_native.c): 0 the original code; 1 a tail call into the region
at that member; 3 the region while g_spu_exp is set (SPU_EXP_AB=1
SPURS_LANE_CYC=1 flips it every lane-timing window: A/B within one boot).
Only with the local-store probes compiled out (SPU_LS_NO_PROBES, the
default), since the probes read ctx->gpr[0].

Measured on body 0x01697100's loop at Eiger Nordwand with
scripts/test/spu_loop_bench.py (captured real states): identical registers
and local store, 1.25-1.45x faster. Keeping the locals as u128 unions
instead was 0.75x (the reason for __m128i).

Run after every SPU lift or --chain, after spu_regpromo.py; generated/ is
not in the repo.

    python scripts/spu_region.py            # patch (idempotent)
    python scripts/spu_region.py --check    # report, change nothing
    python scripts/spu_region.py --restore  # undo
"""
import re
import sys
from pathlib import Path

GEN = Path(__file__).resolve().parent.parent / "generated" / "spu_lifted"
MARK = "/* spu_region */"
BEGIN = "/* spu_region begin */"
END = "/* spu_region end */"
GPR = re.compile(r"ctx->gpr\[(\d+)\]")
GPR_WRITE = re.compile(r"ctx->gpr\[(\d+)\](?:\._\w+\[\d+\])?\s*(?:[-+*/|&^]|<<|>>)?=(?!=)")
LABEL = re.compile(r"\bloc_([0-9A-F]+)\b")

PROLOGUE = [
    f"extern volatile int g_gt5p_spu_region; {MARK}",
    f"int gt5p_spu_region_init(void); {MARK}",
    f"#ifdef SPU_LS_NO_PROBES {MARK}",
    f"#define GT5P_RG_MODE() (g_gt5p_spu_region >= 0 ? g_gt5p_spu_region : gt5p_spu_region_init()) {MARK}",
    f"#else {MARK}",
    f"#define GT5P_RG_MODE() 0 {MARK}",
    f"#endif {MARK}",
    f"#ifndef GT5P_RG_CONV {MARK}",
    f"#define GT5P_RG_CONV {MARK}",
    f"static inline u128 RU(__m128i x) {{ u128 r; memcpy(&r, &x, 16); return r; }} {MARK}",
    f"static inline __m128i RX(u128 x) {{ __m128i r; memcpy(&r, &x, 16); return r; }} {MARK}",
    f"#endif {MARK}",
]


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


def region_lines(image, members, bodies, all_regs=True):
    """The region function for these members (bodies: address -> lines).
    all_regs=False: only the registers the members write become locals; the
    read-only ones stay in ctx->gpr (fewer values for the XMM registers)."""
    allb = [l for m in members for l in bodies[m]]
    if any("&ctx->gpr" in l for l in allb):
        raise ValueError("a member takes the address of a register")
    regs = sorted({int(n) for l in allb for n in GPR.findall(l)})
    written = sorted({int(n) for l in allb for n in GPR_WRITE.findall(l)})
    if not all_regs:
        regs = written
    promoted = set(regs)
    spill = " ".join(f"ctx->gpr[{n}] = RU(r{n});" for n in written) + " "
    tail = re.compile(r"__attribute__\(\(musttail\)\) return (gt5p_%s_spu_func_([0-9A-F]{8}))\(ctx\);" % image)
    name = f"gt5p_{image}_region_{members[0]:08X}"
    out = [f"static void {name}(spu_context* ctx) {{"]
    out += [f"        __m128i r{n} = RX(ctx->gpr[{n}]);" for n in regs]
    out.append("        switch (ctx->pc & 0x3FFFCu) {")
    out += [f"        case 0x{m:X}u: goto R_{m:X};" for m in members]
    out.append(f"        default: {{ {spill}return; }}")
    out.append("        }")
    for m in members:
        out.append(f"R_{m:X}:;")
        for l in bodies[m]:
            # The lifter braces every if body, so statements put in front of
            # a leaving statement stay on its path. Refuse anything else.
            if (re.search(r"(?<!musttail\))\)\s*(return|SPU_RET|__attribute__)", l)
                    or re.search(r"\belse\s+(return|SPU_RET)", l)):
                raise ValueError(f"unbraced leaving statement in 0x{m:X}: {l.strip()[:80]}")
            def only(f):   # rewrite only promoted registers
                return lambda x: f(x) if int(x.group(1)) in promoted else x.group(0)
            l = re.sub(r"ctx->gpr\[(\d+)\]\._u32\[0\]",
                       only(lambda x: f"((uint32_t)_mm_cvtsi128_si32(r{x.group(1)}))"), l)
            l = re.sub(r"ctx->gpr\[(\d+)\]\._u16\[0\]",
                       only(lambda x: f"((uint16_t)_mm_cvtsi128_si32(r{x.group(1)}))"), l)
            if any(int(n) in promoted for n in re.findall(r"ctx->gpr\[(\d+)\]\.", l)):
                raise ValueError(f"lane access not handled in 0x{m:X}: {l.strip()[:80]}")
            l = re.sub(r"^(\s*)ctx->gpr\[(\d+)\] = (.*);$", r"\1r\2 = RX(\3);", l)
            if re.search(r"ctx->gpr\[\d+\]\s*=(?!=)", l):
                raise ValueError(f"register write not handled in 0x{m:X}: {l.strip()[:80]}")
            l = re.sub(r"ctx->gpr\[(\d+)\](?!\.)", only(lambda x: f"RU(r{x.group(1)})"), l)
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
    return name, out, f"{len(members)} functions, {len(regs)} registers ({len(written)} written)"


def patch_image(image, regions, text, all_regs=True):
    """text: the lifted source. regions: lists of member addresses."""
    crlf = "\r\n" in text
    lines = strip(text.replace("\r\n", "\n").split("\n"))
    report, blocks, redirect = [], {}, {}
    for members in regions:
        bodies, heads = {}, {}
        try:
            for m in members:
                head = f"void gt5p_{image}_spu_func_{m:08X}(spu_context* ctx) {{"
                s = lines.index(head)
                e = lines.index("}", s + 1)
                # A member's register-promotion switch (spu_regpromo.py) has no
                # place inside the region: its check mode reads ctx->gpr.
                heads[m], bodies[m] = s, [l for l in lines[s + 1:e] if "/* spu_regpromo */" not in l]
            name, fn, what = region_lines(image, members, bodies, all_regs)
        except ValueError as ex:
            report.append(f"region 0x{members[0]:X}: skipped, {ex or 'member not found'}")
            continue
        blocks[min(heads.values())] = [BEGIN] + fn + [END]
        for m, s in heads.items():
            redirect[s] = (f"        {{ const int _rgm = GT5P_RG_MODE(); if (_rgm == 1 || (_rgm == 3 && g_spu_exp)) {{ "
                           f"ctx->pc = 0x{m:X}u; __attribute__((musttail)) return {name}(ctx); }} }} {MARK}")
        report.append(f"region 0x{members[0]:X}: {what}")
    out = []
    first = min(blocks) if blocks else None
    for i, line in enumerate(lines):
        if i == first:
            out.extend(PROLOGUE)
        if i in blocks:
            out.extend(blocks[i])
        out.append(line)
        if i in redirect:
            out.append(redirect[i])
    new = "\n".join(out)
    return (new.replace("\n", "\r\n") if crlf else new), report


def main():
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import target
    mode = sys.argv[1] if len(sys.argv) > 1 else ""
    want = {}
    for image, members in target.TARGET.get("spu_regions", []):
        want.setdefault(image, []).append([int(a, 16) for a in members])
    bad = 0
    for image in sorted(set(want) | {d.name for d in GEN.glob("*") if d.is_dir()}):
        src = GEN / image / "spu_recomp.c"
        if not src.exists():
            if image in want:
                print(f"  {image}: no lifted source")
                bad += 1
            continue
        raw = src.read_bytes().decode("utf-8")
        if mode == "--restore" or image not in want:
            crlf = "\r\n" in raw
            clean = "\n".join(strip(raw.replace("\r\n", "\n").split("\n")))
            if crlf:
                clean = clean.replace("\n", "\r\n")
            if clean != raw:
                if mode == "--check":
                    print(f"  {image}: carries a merged region but is not listed")
                    continue
                src.write_bytes(clean.encode("utf-8"))
                print(f"  {image}: restored")
            continue
        new, report = patch_image(image, want[image], raw)
        for r in report:
            print(f"  {image} {r}")
        if any("skipped" in r for r in report):
            bad += 1
        if mode == "--check":
            print(f"  {image}: {'patched' if new == raw else 'needs patching'}")
            continue
        if new != raw:
            src.write_bytes(new.encode("utf-8"))
        print(f"  {image}: {len(want[image])} region(s)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
