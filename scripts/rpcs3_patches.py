#!/usr/bin/env python3
"""Convert RPCS3 patch.yml entries into GT5 runtime switches.

Usage:
  python scripts/rpcs3_patches.py <patch.yml> [--target BCUS98114-v104] [--elf EBOOT.ELF] [--list] [--emit <dir>]

Reads an RPCS3 patch.yml (no PyYAML required; supports anchors `name: &name` /
`[ load, *name ]`) and classifies every patch matching the target serial and
version into:
  insn       only whole instruction words (< code_end) -> runtime guard switch
  data       only bytes outside code (>= code_end)     -> guest memory write at boot
  insn+data  both instruction and data replacements
  cave       needs new code (calloc / alloc / jump / jumpl / code) -> manual hook
  config     depends on patch-manager Configurable Values
  empty      no executable patch lines

With `--emit <dir>`, writes:
  <dir>/<target>.insn_patches.json   instruction replacements with guard names
  <dir>/<target>_patches.inc         C/C++ guard definitions, env vars, and data writes
"""
from __future__ import annotations

import argparse
import json
import os
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TARGETS_JSON = ROOT / "config" / "targets.json"

TARGET_VERSIONS = {
    "BCUS98114-v104": "01.04",
    "BCUS98114-v211": "02.11",
    "BCUS98114": "02.00",
    "BCUS98158": "03.00",
    "NPUA80075": "02.00",
}

ITEM_RE = re.compile(
    r'^\s*-\s*\[\s*([a-z0-9]+)\s*,\s*([^,\]]+?)\s*(?:,\s*(.+?))?\s*\]\s*(?:#\s*(.*))?$'
)
ANCHOR_RE = re.compile(r'^\s*([A-Za-z0-9_]+):\s*&([A-Za-z0-9_]+)\s*$')
TITLE_RE = re.compile(r'^  "(.+)":\s*$')
CAVE_KINDS = {"calloc", "jumpl", "jump", "alloc", "code", "load?"}


def slugify(title: str) -> str:
    s = re.sub(r"[^A-Za-z0-9]+", "_", title.strip()).strip("_").lower()
    if not s or s[0].isdigit():
        s = "p_" + s
    return s


def read_patches(path: str | Path, serial: str, version: str):
    """Parse RPCS3 patch.yml entries matching (serial, version) and all anchors."""
    lines = Path(path).read_text(encoding="utf-8", errors="replace").splitlines()
    anchors: dict[str, list[str]] = {}
    i = 0
    while i < len(lines):
        m = ANCHOR_RE.match(lines[i])
        if m:
            body: list[str] = []
            j = i + 1
            while j < len(lines) and (
                ITEM_RE.match(lines[j])
                or not lines[j].strip()
                or lines[j].lstrip().startswith("#")
                or re.match(r"^\s{4,}\S", lines[j])
            ):
                if TITLE_RE.match(lines[j]) or ANCHOR_RE.match(lines[j]):
                    break
                body.append(lines[j])
                j += 1
            anchors[m.group(2)] = body
            i = j
        else:
            i += 1

    out = []
    i = 0
    serial_re = re.compile(r"%s:\s*\[[^\]]*%s" % (re.escape(serial), re.escape(version)))
    while i < len(lines):
        m = TITLE_RE.match(lines[i])
        if not m:
            i += 1
            continue
        title = m.group(1)
        j = i + 1
        block: list[str] = []
        while j < len(lines) and not TITLE_RE.match(lines[j]) and not re.match(r"^\S", lines[j]):
            block.append(lines[j])
            j += 1
        text = "\n".join(block)
        if serial_re.search(text):
            a = re.search(r'Author:\s*"?([^"\n]*)"?', text)
            author = a.group(1).strip() if a else "?"
            cfg_vals: dict[str, str] = {}
            if "Configurable Values:" in text:
                cfg_part = text.split("Configurable Values:", 1)[1]
                if "Patch:" in cfg_part:
                    cfg_part = cfg_part.split("Patch:", 1)[0]
                cur_key = None
                for cln in cfg_part.splitlines():
                    km = re.match(r'^\s{6}"?([^":\n]+?)"?:\s*$', cln)
                    if km:
                        cur_key = km.group(1).strip()
                        continue
                    if cur_key:
                        vm = re.match(r'^\s+Value:\s*"?([^"\n#]+?)"?\s*(?:#.*)?$', cln)
                        if vm:
                            cfg_vals[cur_key] = vm.group(1).strip()
            body = text.split("Patch:", 1)[1].splitlines() if "Patch:" in text else []
            out.append({
                "title": title,
                "author": author,
                "lines": body,
                "configurable": "Configurable Values:" in text,
                "cfg_defaults": cfg_vals,
            })
        i = j
    return out, anchors


def expand_items(lines: list[str], anchors: dict[str, list[str]], depth: int = 0):
    items = []
    for ln in lines:
        m = ITEM_RE.match(ln)
        if not m:
            continue
        kind, a, b, note = m.group(1), m.group(2), m.group(3), m.group(4) or ""
        if kind == "load":
            name = a.lstrip("*")
            if name in anchors and depth < 4:
                items.extend(expand_items(anchors[name], anchors, depth + 1))
            else:
                items.append(("load?", name, None, note))
        else:
            items.append((kind, a, b, note))
    return items


class Elf64BE:
    """Minimal 64-bit big-endian PS3 ELF reader for PT_LOAD word lookups."""

    def __init__(self, path: str | Path):
        self.d = Path(path).read_bytes()
        if len(self.d) < 0x40 or self.d[:4] != b"\x7fELF":
            raise ValueError(f"not an ELF file: {path}")
        phoff, = struct.unpack_from(">Q", self.d, 0x20)
        phentsize, phnum = struct.unpack_from(">HH", self.d, 0x36)
        self.seg: list[tuple[int, int, int]] = []
        for k in range(phnum):
            t, _f, off, va, _pa, fsz, _msz, _al = struct.unpack_from(
                ">IIQQQQQQ", self.d, phoff + k * phentsize
            )
            if t == 1:  # PT_LOAD
                self.seg.append((va, off, fsz))

    def word(self, va: int) -> int | None:
        for a, off, fsz in self.seg:
            if a <= va and va + 4 <= a + fsz:
                return struct.unpack_from(">I", self.d, off + (va - a))[0]
        return None


def classify_patch(
    patch: dict,
    anchors: dict[str, list[str]],
    code_end: int,
    elf: Elf64BE | None = None,
    drop_caves: bool = False,
):
    """Classify one patch -> (kind, insn {addr: new_word}, data [(addr, bytes)], why)."""
    items = expand_items(patch["lines"], anchors)
    if drop_caves:
        kept = []
        in_cave = False
        for it in items:
            if it[0] in ("calloc", "alloc"):
                in_cave = True
                continue
            if in_cave and it[1].strip().lower() in ("0x0", "0"):
                continue
            in_cave = False
            kept.append(it)
        items = kept

    cave_hits = sorted({k for k, *_ in items if k in CAVE_KINDS})
    if cave_hits:
        return "cave", {}, [], "needs new code: " + ", ".join(cave_hits)

    insn: dict[int, int] = {}
    data: list[tuple[int, bytes]] = []
    cfg_defaults = patch.get("cfg_defaults", {})
    for kind, a, b, _note in items:
        try:
            addr = int(a, 0)
        except ValueError:
            return "cave", {}, [], "address is not a number: " + a
        b_val = b
        if isinstance(b_val, str):
            stripped = b_val.strip().strip('"').lstrip("$")
            if stripped in cfg_defaults:
                b_val = cfg_defaults[stripped]
        if kind == "bef32":
            try:
                raw = struct.pack(">f", float(b_val))
            except (TypeError, ValueError):
                return "config", {}, [], "configurable value: " + str(b)
        elif kind in ("be32", "be16", "byte", "be64"):
            try:
                v = int(b_val, 0)
            except (TypeError, ValueError):
                return "config", {}, [], "configurable value: " + str(b)
            raw = {
                "be32": struct.pack(">I", v & 0xFFFFFFFF),
                "be16": struct.pack(">H", v & 0xFFFF),
                "byte": bytes([v & 0xFF]),
                "be64": struct.pack(">Q", v & 0xFFFFFFFFFFFFFFFF),
            }[kind]
        else:
            return "cave", {}, [], "unsupported line type: " + kind

        if addr < code_end:
            for n, byte in enumerate(raw):
                w = (addr + n) & ~3
                if w in insn:
                    cur = insn[w]
                elif elf is not None:
                    cur = elf.word(w)
                    if cur is None:
                        return "cave", {}, [], "0x%08X is not in the ELF image" % w
                else:
                    cur = 0
                sh = (3 - ((addr + n) & 3)) * 8
                insn[w] = (cur & ~(0xFF << sh)) | (byte << sh)
        else:
            data.append((addr, raw))

    if insn and data:
        pkind = "insn+data"
    elif insn:
        pkind = "insn"
    elif data:
        pkind = "data"
    else:
        pkind = "empty"
    return pkind, insn, data, ""


def emit_outputs(
    rows,
    out_dir: str | Path,
    target: str,
    elf: Elf64BE | None,
    only: set[str] | None = None,
):
    out_path = Path(out_dir)
    out_path.mkdir(parents=True, exist_ok=True)
    jp = []
    guards = []
    datas = []

    for p, kind, insn, data, why in rows:
        if only is not None and p["title"] not in only:
            continue
        if kind not in ("insn", "data", "insn+data"):
            if only is not None:
                raise SystemExit(f"{p['title']}: cannot emit ({kind}: {why})")
            continue
        if insn and elf is None:
            raise SystemExit(f"{p['title']}: --elf is required to emit instruction patches")
        slug = slugify(p["title"])
        guard = f"g_gt5_patch_{slug}"
        env = f"GT5_PATCH_{slug.upper()}"
        guards.append((guard, env, p["title"], p["author"]))
        for addr in sorted(insn):
            orig = elf.word(addr)
            if orig != insn[addr]:
                jp.append({
                    "addr": f"0x{addr:08X}",
                    "orig": f"0x{orig:08X}",
                    "new": f"0x{insn[addr]:08X}",
                    "guard": guard,
                    "note": p["title"],
                })
        for addr, raw in data:
            datas.append((guard, addr, raw))

    json_path = out_path / f"{target}.insn_patches.json"
    json_path.write_text(json.dumps({"target": target, "patches": jp}, indent=2) + "\n", encoding="utf-8")

    inc = [
        f"/* Generated by scripts/rpcs3_patches.py for {target} -- do not edit. */",
        'extern "C" {',
    ]
    for guard, *_ in guards:
        inc.append(f"int {guard} = 0;")
    inc.append("}")
    inc.append("static const struct { int* guard; const char* env; const char* title; const char* author; } k_gt5_patches[] = {")
    for guard, env, title, author in guards:
        t_esc = title.replace("\\", "\\\\").replace('"', '\\"')
        a_esc = author.replace("\\", "\\\\").replace('"', '\\"')
        inc.append(f'    {{ &{guard}, "{env}", "{t_esc}", "{a_esc}" }},')
    inc.append("    { nullptr, nullptr, nullptr, nullptr }")
    inc.append("};")
    inc.append("static const struct { int* guard; unsigned addr; unsigned len; const unsigned char* bytes; } k_gt5_patch_data[] = {")
    for guard, addr, raw in datas:
        lit = "".join(f"\\x{b:02X}" for b in raw)
        inc.append(f'    {{ &{guard}, 0x{addr:08X}u, {len(raw)}u, (const unsigned char*)"{lit}" }},')
    inc.append("    { nullptr, 0u, 0u, nullptr }")
    inc.append("};")

    inc_path = out_path / f"{target}_patches.inc"
    inc_path.write_text("\n".join(inc) + "\n", encoding="utf-8", newline="\n")
    return json_path, inc_path, len(jp), len(guards), len(datas)


def load_target_meta(target: str) -> tuple[str, str, int]:
    targets = json.loads(TARGETS_JSON.read_text(encoding="utf-8"))
    if target not in targets:
        raise SystemExit(f"unknown target {target!r}; known: {', '.join(targets)}")
    t = targets[target]
    serial = t.get("title_id", target.split("-", 1)[0])
    version = TARGET_VERSIONS.get(target, "01.00")
    code_end = int(t["code_end"], 0)
    return serial, version, code_end


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Classify and emit RPCS3 patch.yml entries for GT5.")
    ap.add_argument("patch_yml", help="Path to RPCS3 patch.yml")
    ap.add_argument("--target", default="BCUS98114-v104", help="Target key in config/targets.json")
    ap.add_argument("--serial", help="Override title serial (default from target)")
    ap.add_argument("--version", help="Override app version (default from target)")
    ap.add_argument("--code-end", help="Override code_end hex address (default from target)")
    ap.add_argument("--elf", help="Path to decrypted EBOOT.ELF (required for --emit of insn patches)")
    ap.add_argument("--list", action="store_true", help="Print classified patch table")
    ap.add_argument("--emit", metavar="DIR", help="Emit JSON + .inc into DIR")
    ap.add_argument("--only", help="Comma-separated list of patch titles to emit")
    ap.add_argument("--drop-caves", action="store_true", help="Keep plain replacements from patches that also have caves")
    args = ap.parse_args(argv)

    serial, version, code_end = load_target_meta(args.target)
    if args.serial:
        serial = args.serial
    if args.version:
        version = args.version
    if args.code_end:
        code_end = int(args.code_end, 0)

    elf = Elf64BE(args.elf) if args.elf else None
    patches, anchors = read_patches(args.patch_yml, serial, version)
    rows = [
        (p, *classify_patch(p, anchors, code_end, elf=elf, drop_caves=args.drop_caves))
        for p in patches
    ]

    if args.list or not args.emit:
        print(f"Target {args.target} ({serial} v{version}, code_end=0x{code_end:08X}): {len(rows)} matching patches")
        for p, kind, insn, data, why in rows:
            dbytes = sum(len(b) for _, b in data)
            extra = f"   ({why})" if why else ""
            print(f"  {kind:9} {len(insn):3} insn {dbytes:5} data bytes  {p['title']}  -- {p['author']}{extra}")

    if args.emit:
        only = {s.strip() for s in args.only.split(",") if s.strip()} if args.only else None
        jp, inc, nj, ng, nd = emit_outputs(rows, args.emit, args.target, elf, only=only)
        print(f"wrote {jp} ({nj} instruction patches) and {inc} ({ng} switches, {nd} data writes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
