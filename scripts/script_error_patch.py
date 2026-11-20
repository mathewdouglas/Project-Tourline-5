#!/usr/bin/env python3
"""Log the game's script errors before it acts on them.

GT5's script engine reports an error through one routine (`script_error_func`
in config/targets.json: 2.11 0x009C3048, 01.04 0x00B3FE1C). In its throwing
mode that routine throws a guest C++ exception, which the port cannot deliver
to a handler: the game then terminates with exit code 1 and the log never
said which error it was (#55/#56/#57). This puts a call to gt5p_script_error
(src/gt5p_script_error.cpp) at the routine's entry, which prints

    [script-error] FatalError m_adhoc.cpp:864: nil object cannot be used as ...

GT5P_SCRIPT_ERROR_LOG=0 at run time prints nothing.

Required post-lift step; run after every re-lift, with spin_patch.py.
generated/ is not in the repo.

    python scripts/script_error_patch.py            # patch (idempotent)
    python scripts/script_error_patch.py --check    # report, change nothing
    python scripts/script_error_patch.py --restore  # undo
"""
import re
import sys

import target
from spin_patch import find_function

MARK = "/* script_error_patch */"
DECL = ('extern "C" void gt5p_script_error(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t); '
        + MARK + "\n")
CALL = ("        gt5p_script_error(ctx->gpr[3], ctx->gpr[4], ctx->gpr[5], ctx->gpr[6], ctx->gpr[7]); "
        + MARK + "\n")


def patch_body(body):
    first = body.index("{\n") + 2
    return body[:first] + CALL + body[first:]


def unpatch_body(body):
    return body.replace(CALL, "")


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else ""
    func_s = target.TARGET.get("script_error_func")
    if not func_s:
        print(f"no script_error_func for {target.NAME}")
        return
    func = "%08X" % int(func_s, 16)
    f, text, start, end = find_function(func)
    body = text[start:end]
    patched = MARK in body
    if mode == "--check":
        print(f"func_{func} ({f.name}): {'patched' if patched else 'NOT patched'}")
        return
    if mode == "--restore":
        if not patched:
            print(f"func_{func}: not patched")
            return
        text = text[:start] + unpatch_body(body) + text[end:]
        if MARK not in text.replace(DECL, ""):
            text = text.replace(DECL, "")
        print(f"func_{func}: restored")
    else:
        if patched:
            print(f"func_{func}: already patched")
            return
        text = text[:start] + patch_body(body) + text[end:]
        if DECL not in text:
            last = 0
            for m in re.finditer(r"^#include [^\n]*\n", text, re.M):
                if m.start() > 4000:
                    break
                last = m.end()
            text = text[:last] + DECL + text[last:]
        print(f"func_{func}: logs script errors")
    f.write_text(text, encoding="utf-8", errors="surrogateescape", newline="")


if __name__ == "__main__":
    main()
