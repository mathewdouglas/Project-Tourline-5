"""Check the shipped audio patch using a private Git index; no runtime edits/device access."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, help="test an existing cellAudio.c instead")
    args = parser.parse_args()
    manifest = json.loads((ROOT / "config/ps3recomp.json").read_text())
    runtime = Path(os.environ.get("PS3RECOMP_DIR", ROOT.parent / manifest["directory"]))
    (ROOT / "scratch").mkdir(exist_ok=True)
    folder = Path(tempfile.mkdtemp(prefix="audio-silence-", dir=ROOT / "scratch"))
    if args.source:
        source = args.source.read_text()
    else:
        # Materialize the baseline with the Windows checkout's line endings;
        # the compatibility patch preserves those raw bytes. Never touch the
        # runtime's real index or working tree.
        env = dict(os.environ, GIT_INDEX_FILE=str(folder / "runtime.index"))
        command = ["git", "-C", str(runtime)]
        subprocess.run(command + ["read-tree", manifest["commit"]], env=env, check=True)
        staged = folder / "runtime"
        staged.mkdir()
        subprocess.run(command + ["-c", "core.autocrlf=true", "checkout-index", "--all",
                                  f"--prefix={staged.as_posix()}/"], env=env, check=True)
        subprocess.run(["git", "apply", "--unsafe-paths", "--whitespace=nowarn",
                        f"--directory={staged.as_posix()}", str(ROOT / manifest["patch"])],
                       cwd=ROOT, check=True)
        source = (staged / "libs/audio/cellAudio.c").read_text()
    helper = re.search(r"static int audio_block_has_data\(.*?\n\}", source, re.S)
    if not helper:
        raise SystemExit("Runtime audio silence predicate not found")
    (folder / "audio_silence_helper.inc").write_text(helper.group() + "\n")
    batch = folder / "build.cmd"
    batch.write_text(
        '@echo off\nset "PATH=C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer;%PATH%"\n'
        'call "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\n'
        'if errorlevel 1 exit /b 1\n'
        '"C:\\Program Files\\LLVM\\bin\\clang-cl.exe" /nologo /O2 /MD '
        f'/I"{folder}" /Fo"{folder}/test.obj" /Fe"{folder}/test.exe" '
        f'"{ROOT}/scripts/test/audio_silence.c"\n', encoding="ascii")
    subprocess.run(["cmd", "/c", str(batch)], check=True)
    subprocess.run([str(folder / "test.exe")], check=True, timeout=10)


if __name__ == "__main__":
    main()
