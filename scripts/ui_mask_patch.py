#!/usr/bin/env python3
"""Centre the menus' text fade/clip masks in ultrawide.

The text renderer turns a text box's rectangle, given in the menus' 16:9
layout pixels, into masks its vertex program reads in screen space (NDC):
x0 = rect.x / (W/2) - 1, with W the screen width it works in. That assumes the
layout fills the screen. With the ultrawide UI aspect the menus are laid out
16:9 and centred in a wider W (2560 at 64/27), the menus' projection says so
(x offset -0.75), but these subtractions still use 1.0: the mask sits a
quarter of the screen to the left, so text fades out or is cut off early.
The right offset is the layout's share of the screen, (H * 16/9) / W, which is
exactly 1 at 16:9.

- Prologue (BCUS98158, func_00768340): the My Page labels' fade mask (c[131]);
  "2P Battle" cut, "Events" missing.
- GT5 (func_00A5D384/94 on 01.04, func_008F53B8 on 2.11): the same label mask
  (c[131]) and a clip rectangle with soft edges (c[133], slopes c[134]): the
  "Information" popups' text cut off mid-line (GT Mode's first My Home and
  Dealerships messages).

Each listed subtraction `fpr[x] - fpr[one]` becomes a call to gt5p_ui_mask_x
(src/gt5p_uifix.cpp), handed the width and height registers (whole or halved:
only their ratio is used). GT5P_UI_MASK_FIX=0 at run time keeps 1.0.
Config in config/targets.json: "ui_mask_sites", a list of
{"func", "x": [registers], "one", "w", "h"}; Prologue's older
"ui_label_mask_func" is the site {func, x: [13], one: 31, w: 30, h: 29}.

Run after every re-lift, with setjmp_patch.py and spin_patch.py; generated/
is not in the repo.

    python scripts/ui_mask_patch.py            # patch (idempotent)
    python scripts/ui_mask_patch.py --check    # report, change nothing
    python scripts/ui_mask_patch.py --restore  # undo
"""
import re
import sys
from pathlib import Path

import target

GEN = Path(__file__).resolve().parent.parent / "generated"
MARK = "/* ui_mask_patch */"
DECL = 'extern "C" double gt5p_ui_mask_x(double w, double h, double one); ' + MARK + "\n"


def sites():
    t = target.TARGET
    out = list(t.get("ui_mask_sites", []))
    if t.get("ui_label_mask_func"):
        out.append({"func": t["ui_label_mask_func"], "x": [13], "one": 31, "w": 30, "h": 29})
    return out


def lines(site, x):
    one, w, h = site["one"], site["w"], site["h"]
    orig = f"        ctx->fpr[{x}] = ppu_fp_single(ppu_fsub(ctx->fpr[{x}], ctx->fpr[{one}]));\n"
    new = (f"        ctx->fpr[{x}] = ppu_fp_single(ppu_fsub(ctx->fpr[{x}], "
           f"gt5p_ui_mask_x(ctx->fpr[{w}], ctx->fpr[{h}], ctx->fpr[{one}]))); {MARK}\n")
    return orig, new


CRLF = set()   # files whose lift wrote CRLF line endings: written back so


def read(f):
    with open(f, encoding="utf-8", errors="surrogateescape", newline="") as h:
        text = h.read()
    if "\r\n" in text[:4000]:
        CRLF.add(f)
        text = text.replace("\r\n", "\n")
    return text


def add_decl(text):
    if DECL in text:
        return text
    last = 0
    for m in re.finditer(r"^#include [^\n]*\n", text, re.M):
        if m.start() > 4000:
            break
        last = m.end()
    return text[:last] + DECL + text[last:]


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else ""
    todo = sites()
    if not todo:
        print(f"no ui_mask_sites for {target.NAME}")
        return
    files = sorted(GEN.glob("ppu_recomp_*.cpp"))
    texts = {}
    for site in todo:
        head = "void func_%08X(ppu_context* ctx) {" % int(site["func"], 16)
        name = head[5:18]
        for f in files:
            text = texts.get(f) or read(f)
            start = text.find(head)
            if start >= 0:
                break
        else:
            raise SystemExit(f"{head} not found in generated/")
        end = text.find("\n}\n", start) + 3
        body = text[start:end]
        pairs = [lines(site, x) for x in site["x"]]
        done = sum(body.count(n) for _, n in pairs)
        if mode == "--check":
            print(f"{name} ({f.name}): {'patched' if done == len(pairs) else 'NOT patched'}")
            continue
        if mode == "--restore":
            if not done:
                print(f"{name}: not patched")
                continue
            for o, n in pairs:
                body = body.replace(n, o)
            print(f"{name}: restored")
        else:
            if done == len(pairs):
                print(f"{name}: already patched")
                continue
            for o, n in pairs:
                if body.count(o) != 1:
                    raise SystemExit(f"{name}: expected `{o.strip()}` once, found {body.count(o)}")
                body = body.replace(o, n)
            print(f"{name}: {len(pairs)} mask subtraction(s) patched")
        texts[f] = text[:start] + body + text[end:]
    for f, text in texts.items():
        if MARK in text.replace(DECL, ""):
            text = add_decl(text)
        else:
            text = text.replace(DECL, "")
        if f in CRLF:
            text = text.replace("\n", "\r\n")
        f.write_text(text, encoding="utf-8", errors="surrogateescape", newline="")


if __name__ == "__main__":
    main()
