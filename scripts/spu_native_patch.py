#!/usr/bin/env python3
"""Hook native replacements into lifted SPU job code.

Each entry of "spu_native_hooks" in config/targets.json is
[image directory, block address, host function]: the lifted block
generated/spu_lifted/<image>/spu_recomp.c:..._spu_func_<block> gets a call to
that function (src/gt5p_spu_native.c) as its first statement. The function
may advance the guest state past work it does natively; it must leave the
lifted block to finish so every register ends as the lifted code leaves it.
GT5P_SPU_NATIVE at run time: 0 off, 1 on (default), 2 check (compute natively,
compare against what the lifted code then does, change nothing).

A fourth element makes a jump hook: [image, block, int fn(ctx, fused), target]. When the
function returns nonzero it has done the block's work up to the target block itself, and
the lifted code goes on there; zero runs the lifted block as it is. `fused` says whether
this copy of the image computes fma/fms/fnms fused (an FMA copy) or through doubles.

Run after every SPU lift or --chain, with spin_patch.py; generated/ is not in
the repo.

"spu_audio_round_zero_calls" scopes measured coefficient-constructor calls
with the SPU's truncating mode, restoring the caller's MXCSR afterwards.
The matching image must force-include gt5p_spu_audio_math.h (CMake).

    python scripts/spu_native_patch.py            # patch (idempotent)
    python scripts/spu_native_patch.py --check    # report, change nothing
    python scripts/spu_native_patch.py --restore  # undo
"""
import sys
from pathlib import Path

import target

GEN = Path(__file__).resolve().parent.parent / "generated" / "spu_lifted"
MARK = "/* spu_native_patch */"


def wrap_audio_round_calls(lines, image, calls):
    """Scope measured constructor calls, including any drained continuation.

    Wrapper lines carry MARK; the original lifted line stays byte-for-byte
    intact so stripping marked lines also restores these scopes.
    """
    out, found = [], {block: 0 for block, _ in calls}
    for line in lines:
        block = next((b for b, _ in calls
                      if f"gt5p_{image}_spu_func_{b:08X}(ctx);" in line
                      and "ctx->host_depth++;" in line and "spu_drain_call(ctx," in line), None)
        if block is not None:
            out.append(f"        {{ unsigned _spu_audio_mxcsr = gt5p_spu_audio_round_enter(); {MARK}")
            found[block] += 1
        out.append(line)
        if block is not None:
            out.append(f"        gt5p_spu_audio_round_leave(_spu_audio_mxcsr); }} {MARK}")
    missing = [f"round scope {b:#x}: {found[b]} calls, expected {n}"
               for b, n in calls if found[b] != n]
    return out, missing


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else ""
    hooks = target.TARGET.get("spu_native_hooks", [])
    by_image = {}
    jumps = {}
    for hook in hooks:
        image, block, fn = hook[:3]
        by_image.setdefault(image, []).append((int(block, 16), fn))
        if len(hook) > 3:   # a jump hook: fn(ctx, fused) returns nonzero to go on at that block (or "RET")
            jumps[(image, int(block, 16))] = "RET" if hook[3] == "RET" else int(hook[3], 16)
    round_calls = {}
    for image, block, count in target.TARGET.get("spu_audio_round_zero_calls", []):
        round_calls.setdefault(image, []).append((int(block, 16), count))
    bad = 0
    for image in sorted(set(by_image) | {d.name for d in GEN.glob("*") if d.is_dir()}):
        src = GEN / image / "spu_recomp.c"
        if not src.exists():
            continue
        text = src.read_text(encoding="utf-8")
        lines = text.split("\n")
        clean = [l for l in lines if MARK not in l]
        if mode == "--restore":
            if len(clean) != len(lines):
                src.write_text("\n".join(clean), encoding="utf-8", newline="\n")
                print(f"  {image}: restored")
            continue
        want = by_image.get(image, [])
        want_round = round_calls.get(image, [])
        if not want and not want_round:
            continue
        out, done = [], set()
        decls = []
        for block, fn in dict.fromkeys(want):
            if (image, block) in jumps:
                decls.append(f"int {fn}(spu_context* ctx, int fused); {MARK}")
            else:
                decls.append(f"void {fn}(spu_context* ctx); {MARK}")
        if any(img == image for img, _ in jumps):
            # Whether this copy's fma/fms/fnms are fused (the FMA copies) or computed through
            # doubles (spu_helpers.h SPU__FMAF): a jump hook's native code does the same.
            decls[:0] = [f"#if defined(__FMA__) || defined(SPU_FMA_LIBM) {MARK}",
                         f"#define GT5P_HOOK_FUSED 1 {MARK}", f"#else {MARK}",
                         f"#define GT5P_HOOK_FUSED 0 {MARK}", f"#endif {MARK}"]
        last_inc = max((i for i, l in enumerate(clean[:40]) if l.startswith("#include")), default=-1)
        for i, line in enumerate(clean):
            out.append(line)
            if i == last_inc:
                out.extend(decls)
                decls = []
            for block, fn in want:
                head = f"void gt5p_{image}_spu_func_{block:08X}(spu_context* ctx) {{"
                if line == head:
                    to = jumps.get((image, block))
                    if to is None:
                        out.append(f"        {fn}(ctx); {MARK}")
                    elif to == "RET":
                        out.append(f"        if ({fn}(ctx, GT5P_HOOK_FUSED)) {{ SPU_RET(ctx); }} {MARK}")
                    else:
                        tgt = f"gt5p_{image}_spu_func_{to:08X}"
                        out.append(f"        if ({fn}(ctx, GT5P_HOOK_FUSED)) {{ ctx->pc = 0x{to:X}u; "
                                   f"__attribute__((musttail)) return {tgt}(ctx); }} {MARK}")
                    done.add(block)
        missing = [hex(b) for b, _ in want if b not in done]
        out, round_missing = wrap_audio_round_calls(out, image, want_round)
        missing.extend(round_missing)
        if decls:
            missing.append("no #include line for the declarations")
        if missing:
            bad += 1
            print(f"  {image}: NOT FOUND {missing}")
            continue
        new = "\n".join(out)
        if mode == "--check":
            print(f"  {image}: {'patched' if new == text else 'needs patching'} ({len(want)} hooks, {sum(n for _, n in want_round)} round scopes)")
            continue
        if new != text:
            src.write_text(new, encoding="utf-8", newline="\n")
        print(f"  {image}: {len(want)} hooks, {sum(n for _, n in want_round)} round scopes")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
