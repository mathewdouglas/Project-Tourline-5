#!/usr/bin/env python3
"""Deliver guest C++ exceptions to chosen catch handlers.

The lifted libgcc unwinder cannot branch into a landing pad in another lifted
function (__builtin_eh_return is lifted as a return), so every guest throw
ends in terminate. src/gt5p_guest_eh.cpp does the search itself and delivers
to "catch sites" this script prepares (`guest_eh` in config/targets.json):

  - `raise_func` (_Unwind_RaiseException) gets a call to gt5p_eh_raise at its
    entry, with the address and size of the game's .eh_frame and the
    __si_class_type_info vtable that typeinfo objects point at;
  - for each site, the try region's call (`call`, in function `func`) is
    wrapped in an armed host setjmp, and a label is put at the landing pad
    (`pad`): the first line equal to `pad_after` after the call ends the code
    before it; the line after it must contain `pad_check`.

GT5 2.11's site is the script engine's native-call wrapper (0x009DA95C): its
catch(HObject) is where script errors (HException) go on a console (#55-#58,
#64). GT5P_GUEST_EH=0 at run time delivers nothing.

Required post-lift step for targets with `guest_eh`; run after every re-lift.
generated/ is not in the repo.

    python scripts/guest_eh_patch.py            # patch (idempotent)
    python scripts/guest_eh_patch.py --check    # report, change nothing
    python scripts/guest_eh_patch.py --restore  # undo
"""
import re
import sys

import target
from spin_patch import find_function

MARK = "/* guest_eh_patch */"
MARK_END = "/* guest_eh_patch end */"
DECL = ("#include <setjmp.h>\n"
        "struct gt5p_eh_site; "
        'extern "C" gt5p_eh_site* gt5p_eh_arm(ppu_context*, uint32_t); '
        'extern "C" jmp_buf* gt5p_eh_host(gt5p_eh_site*); '
        'extern "C" void gt5p_eh_disarm(gt5p_eh_site*); '
        'extern "C" void gt5p_eh_raise(ppu_context*, uint32_t, uint32_t, uint32_t); ' + MARK + "\n")


def hexaddr(s):
    return int(s, 16) if isinstance(s, str) else int(s)


def add_decl(text):
    if DECL in text:
        return text
    last = 0
    for m in re.finditer(r"^#include [^\n]*\n", text, re.M):
        if m.start() > 4000:
            break
        last = m.end()
    return text[:last] + DECL + text[last:]


def drop_decl(text):
    if MARK not in text.replace(DECL, ""):
        text = text.replace(DECL, "")
    return text


def raise_call(cfg):
    return ("        gt5p_eh_raise(ctx, 0x%08Xu, 0x%Xu, 0x%08Xu); %s\n"
            % (hexaddr(cfg["eh_frame"]), hexaddr(cfg["eh_frame_size"]),
               hexaddr(cfg["si_typeinfo_vtable"]), MARK))


def patch_site(body, site):
    func, call, pad = (hexaddr(site[k]) for k in ("func", "call", "pad"))
    call_re = re.compile(r"^( *)(ctx->lr = 0x%08X; func_[0-9A-F]{8}\(ctx\); DRAIN_TRAMPOLINE\(ctx\);)\n"
                         % (call + 4), re.M)
    m = call_re.search(body)
    if not m:
        raise SystemExit(f"site 0x{func:08X}: call at 0x{call:08X} not found")
    ind, line = m.group(1), m.group(2)
    rest = body[m.end():]
    label = "loc_%08X" % pad
    if label + ":" in body:
        raise SystemExit(f"site 0x{func:08X}: {label} already exists")
    if "pad_body" in site:
        # Cold landing pad was not emitted in this lifted function (the lifter
        # stopped at an unconditional branch before it); append it before '}'.
        close = rest.rfind("\n}")
        if close < 0:
            raise SystemExit(f"site 0x{func:08X}: closing brace not found for pad_body")
        pad_lines = [f"{label}: {MARK}"] + [f"{ind}{ln}" for ln in site["pad_body"]] + [MARK_END]
        rest = rest[:close + 1] + "\n".join(pad_lines) + "\n" + rest[close + 1:]
    else:
        k = rest.find(site["pad_after"] + "\n")
        if k < 0:
            raise SystemExit(f"site 0x{func:08X}: '{site['pad_after']}' not found after the call")
        k += len(site["pad_after"]) + 1
        nxt = rest[k:rest.find("\n", k)]
        if site["pad_check"] not in nxt:
            raise SystemExit(f"site 0x{func:08X}: line after '{site['pad_after']}' is '{nxt.strip()}', "
                             f"expected '{site['pad_check']}'")
        rest = rest[:k] + f"{label}: {MARK}\n" + rest[k:]
    wrapped = (f"{ind}{{ gt5p_eh_site* _eh = gt5p_eh_arm(ctx, 0x{func:08X}u); {MARK}\n"
               f"{ind}  if (setjmp(*gt5p_eh_host(_eh)) != 0) goto {label};\n"
               f"{ind}  {line}\n"
               f"{ind}  gt5p_eh_disarm(_eh); }}\n")
    return body[:m.start()] + wrapped + rest


def unpatch_site(body, site):
    func, pad = hexaddr(site["func"]), hexaddr(site["pad"])
    if "pad_body" in site:
        body = re.sub(r"loc_%08X: %s\n.*?%s\n" % (pad, re.escape(MARK), re.escape(MARK_END)),
                      "", body, flags=re.S)
    else:
        body = body.replace("loc_%08X: %s\n" % (pad, MARK), "")
    m = re.search(r"^( *)\{ gt5p_eh_site\* _eh = gt5p_eh_arm\(ctx, 0x%08Xu\); [^\n]*\n"
                  r" *if \(setjmp[^\n]*\n *([^\n]*)\n *gt5p_eh_disarm\(_eh\); \}\n" % func, body, re.M)
    if m:
        body = body[:m.start()] + m.group(1) + m.group(2) + "\n" + body[m.end():]
    return body


def edit(func_s, mode, patch, unpatch):
    func = "%08X" % hexaddr(func_s)
    f, text, start, end = find_function(func)
    body = text[start:end]
    patched = MARK in body
    if mode == "--check":
        print(f"func_{func} ({f.name}): {'patched' if patched else 'NOT patched'}")
        return patched
    if mode == "--restore":
        if patched:
            text = drop_decl(text[:start] + unpatch(body) + text[end:])
            f.write_text(text, encoding="utf-8", errors="surrogateescape", newline="")
            print(f"func_{func}: restored")
        return True
    if patched:
        print(f"func_{func}: already patched")
        return True
    text = add_decl(text[:start] + patch(body) + text[end:])
    f.write_text(text, encoding="utf-8", errors="surrogateescape", newline="")
    print(f"func_{func}: patched")
    return True


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else ""
    cfg = target.TARGET.get("guest_eh")
    if not cfg:
        print(f"no guest_eh for {target.NAME}")
        return 0
    ok = True
    call = raise_call(cfg)
    ok &= edit(cfg["raise_func"], mode,
               lambda b: b[:b.index("{\n") + 2] + call + b[b.index("{\n") + 2:],
               lambda b: b.replace(call, ""))
    for site in cfg["catch_sites"]:
        ok &= edit(site["func"], mode, lambda b, s=site: patch_site(b, s), lambda b, s=site: unpatch_site(b, s))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
