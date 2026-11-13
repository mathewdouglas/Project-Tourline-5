"""Wrap scripts/test/vp_interp.hlsl as a C string for the vertex interpreter.

The string is the shader body. Callers prepend `#define RSX_VP_INTERP_FETCH`.
Writes scratch/runtime-src/libs/video/rsx_vp_interp_hlsl.inc. That file is
original interpreter source, not a game program, and it reaches git only
inside patches/ps3recomp-shader-interp.patch.
"""
import os
import pathlib

ROOT = pathlib.Path(__file__).resolve().parents[2]
SRC = ROOT / "scripts" / "test" / "vp_interp.hlsl"
# INTERP_OUT_DIR: where to write (default scratch/runtime-src/libs/video).
DST = pathlib.Path(os.environ.get("INTERP_OUT_DIR") or ROOT / "scratch" / "runtime-src" / "libs" / "video") / "rsx_vp_interp_hlsl.inc"


def c_string(text: str) -> str:
    lines = ['static const char rsx_vp_interp_hlsl[] =']
    for line in text.splitlines():
        esc = line.replace("\\", "\\\\").replace('"', '\\"')
        lines.append('"' + esc + '\\n"')
    lines.append(";")
    return "\n".join(lines) + "\n"


def main() -> None:
    body = SRC.read_text(encoding="utf-8")
    if "\0" in body:
        raise SystemExit("shader contains a NUL")
    DST.parent.mkdir(parents=True, exist_ok=True)
    DST.write_text(c_string(body), encoding="utf-8", newline="\n")
    print(f"{DST} ({DST.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
