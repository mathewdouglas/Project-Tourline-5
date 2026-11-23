#!/usr/bin/env python3
"""Unit test for scripts/rpcs3_patches.py using synthetic ELF and patch.yml."""
from __future__ import annotations

import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "rpcs3_patches.py"


def make_synthetic_elf(path: Path) -> None:
    """Build a minimal 64-bit BE ELF with one PT_LOAD at VA 0x00010000 (size 0x200)."""
    ehdr_size = 64
    phdr_size = 56
    seg_off = 0x100
    seg_va = 0x00010000
    seg_size = 0x200

    ehdr = bytearray(ehdr_size)
    ehdr[0:4] = b"\x7fELF"
    ehdr[4] = 2  # ELFCLASS64
    ehdr[5] = 2  # ELFDATA2MSB
    struct.pack_into(">Q", ehdr, 0x20, ehdr_size)  # e_phoff
    struct.pack_into(">HH", ehdr, 0x36, phdr_size, 1)  # e_phentsize, e_phnum

    phdr = struct.pack(">IIQQQQQQ", 1, 5, seg_off, seg_va, seg_va, seg_size, seg_size, 0x1000)

    code = bytearray(seg_size)
    # 0x00010000: 0x38600001 (li r3, 1)
    struct.pack_into(">I", code, 0x00, 0x38600001)
    # 0x00010004: 0x38800002 (li r4, 2)
    struct.pack_into(">I", code, 0x04, 0x38800002)
    # 0x00010008: 0x60000000 (nop)
    struct.pack_into(">I", code, 0x08, 0x60000000)

    blob = bytearray(seg_off + seg_size)
    blob[0:ehdr_size] = ehdr
    blob[ehdr_size : ehdr_size + phdr_size] = phdr
    blob[seg_off : seg_off + seg_size] = code
    path.write_bytes(bytes(blob))


SAMPLE_YML = """Version: 1.2
Anchors:
  shared_nop: &shared_nop
    - [ be32, 0x00010000, 0x60000000 ]

PPU-deadbeef:
  "Disable Bloom":
    Games:
      "Gran Turismo 5":
        BCUS98114: [ 01.04 ]
    Author: "Tester"
    Patch:
      - [ load, *shared_nop ]
      - [ be16, 0x00010006, 0x0000 ]
  "Tune Camera Float":
    Games:
      "Gran Turismo 5":
        BCUS98114: [ 01.04, 02.11 ]
    Author: "Tester"
    Patch:
      - [ bef32, 0x01200000, 1.5 ]
      - [ byte, 0x01200004, 0x7F ]
  "Hybrid Patch":
    Games:
      "Gran Turismo 5":
        BCUS98114: [ 01.04 ]
    Author: "Tester"
    Patch:
      - [ be32, 0x00010008, 0x4E800020 ]
      - [ be64, 0x01200010, 0x0102030405060708 ]
  "Needs Code Cave":
    Games:
      "Gran Turismo 5":
        BCUS98114: [ 01.04 ]
    Author: "Tester"
    Patch:
      - [ calloc, 0x100 ]
      - [ be32, 0x0, 0x60000000 ]
  "Configurable With Default":
    Games:
      "Gran Turismo 5":
        BCUS98114: [ 01.04 ]
    Author: "Tester"
    Configurable Values:
      "Spring Damper":
        Type: double_range
        Value: 90.0
    Patch:
      - [ bef32, 0x01200030, "Spring Damper" ]
  "Configurable FOV":
    Games:
      "Gran Turismo 5":
        BCUS98114: [ 01.04 ]
    Author: "Tester"
    Configurable Values:
      fov: 60.0
    Patch:
      - [ bef32, 0x01200020, "$fov" ]
  "Other Version Only":
    Games:
      "Gran Turismo 5":
        BCUS98114: [ 02.11 ]
    Author: "Tester"
    Patch:
      - [ be32, 0x00010000, 0x60000000 ]
"""


def main() -> int:
    with tempfile.TemporaryDirectory() as td:
        td_p = Path(td)
        elf = td_p / "EBOOT.ELF"
        yml = td_p / "patch.yml"
        out = td_p / "out"
        make_synthetic_elf(elf)
        yml.write_text(SAMPLE_YML, encoding="utf-8")

        # 1. --list + --emit for v104
        res = subprocess.run(
            [sys.executable, str(SCRIPT), str(yml), "--target", "BCUS98114-v104",
             "--elf", str(elf), "--list", "--emit", str(out)],
            capture_output=True, text=True, check=True,
        )
        stdout = res.stdout
        assert "6 matching patches" in stdout, stdout
        assert "Other Version Only" not in stdout, stdout
        assert "insn        2 insn" in stdout and "Disable Bloom" in stdout, stdout
        assert "data        0 insn     5 data bytes  Tune Camera Float" in stdout, stdout
        assert "data        0 insn     4 data bytes  Configurable With Default" in stdout, stdout
        assert "insn+data   1 insn     8 data bytes  Hybrid Patch" in stdout, stdout
        assert "cave" in stdout and "Needs Code Cave" in stdout, stdout
        assert "config" in stdout and "Configurable FOV" in stdout, stdout

        jp = json.loads((out / "BCUS98114-v104.insn_patches.json").read_text(encoding="utf-8"))
        assert jp["target"] == "BCUS98114-v104"
        by_addr = {p["addr"]: p for p in jp["patches"]}
        assert by_addr["0x00010000"]["orig"] == "0x38600001"
        assert by_addr["0x00010000"]["new"] == "0x60000000"
        assert by_addr["0x00010000"]["guard"] == "g_gt5_patch_disable_bloom"
        # Sub-word be16 at 0x00010006 cleared low 16 bits of 0x38800002 -> 0x38800000
        assert by_addr["0x00010004"]["orig"] == "0x38800002"
        assert by_addr["0x00010004"]["new"] == "0x38800000"
        assert by_addr["0x00010008"]["new"] == "0x4E800020"
        assert by_addr["0x00010008"]["guard"] == "g_gt5_patch_hybrid_patch"

        inc = (out / "BCUS98114-v104_patches.inc").read_text(encoding="utf-8")
        assert "int g_gt5_patch_disable_bloom = 0;" in inc
        assert "GT5_PATCH_TUNE_CAMERA_FLOAT" in inc
        assert "0x01200000u, 4u" in inc
        assert "\\x3F\\xC0\\x00\\x00" in inc  # 1.5f BE

        # 2. --list for v211 (no --elf needed)
        res2 = subprocess.run(
            [sys.executable, str(SCRIPT), str(yml), "--target", "BCUS98114-v211", "--list"],
            capture_output=True, text=True, check=True,
        )
        assert "2 matching patches" in res2.stdout, res2.stdout
        assert "Tune Camera Float" in res2.stdout
        assert "Other Version Only" in res2.stdout

        # 3. End-to-end ppu_lifter.py --insn-patches on the synthetic ELF
        import os
        runtime = Path(os.environ.get("PS3RECOMP_DIR") or (ROOT.parent / "ps3recomp-tourline5"))
        lifter = runtime / "tools" / "ppu_lifter.py"
        if lifter.exists():
            funcs_json = td_p / "funcs.json"
            funcs_json.write_text(json.dumps([{"start": "0x00010000", "end": "0x00010010"}]), encoding="utf-8")
            lift_out = td_p / "lifted"
            subprocess.run(
                [sys.executable, str(lifter), str(elf), "--functions", str(funcs_json),
                 "--insn-patches", str(out / "BCUS98114-v104.insn_patches.json"),
                 "--single-file", "-o", str(lift_out)],
                capture_output=True, text=True, check=True,
            )
            hdr = (lift_out / "ppu_recomp.h").read_text(encoding="utf-8")
            src = (lift_out / "ppu_recomp.c").read_text(encoding="utf-8")
            assert "extern int g_gt5_patch_disable_bloom;" in hdr, hdr
            assert "extern int g_gt5_patch_hybrid_patch;" in hdr, hdr
            assert "if (__builtin_expect(!g_gt5_patch_disable_bloom, 1))" in src, src
            assert "if (__builtin_expect(g_gt5_patch_disable_bloom, 0))" in src, src
            assert "if (__builtin_expect(g_gt5_patch_hybrid_patch, 0)) { return; }" in src, src

    print("rpcs3_patches: all assertions passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
