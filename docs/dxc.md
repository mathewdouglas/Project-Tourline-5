# DXC guest shader compiler

GT5 can compile its translated vertex and fragment shaders with Microsoft's
DirectX Shader Compiler (DXC), targeting shader model 6.0. **FXC remains the
default** until the GT5 output differences below have been resolved or accepted.
GT5's address tables, SPU code and rendering fixes are unchanged.

From the repository root:

```powershell
.venv\Scripts\python scripts/get_dxc.py
scripts\build.cmd -DGT5P_TARGET=BCUS98114-v104
```

The fetcher verifies the official **v1.8.2502** archive and each extracted file
by SHA-256. CMake copies `dxcompiler.dll` and `dxil.dll` beside `build/gt5.exe`
and their notices to `build/licenses/dxc`. Binaries stay gitignored. Downloads
are explicit; a build does not download dependencies. `--zip FILE` supports a
previously downloaded pinned archive.

Select **F1 > Shaders > Shader compiler > DXC**, then restart. The equivalent
setting is:

```toml
[video]
shader_compiler = "dxc"
```

`RSX_SHADER_COMPILER=dxc` overrides the file. Set it to `fxc`, or choose FXC in
F1 and restart, to roll back. Unknown environment selections use FXC. Missing
compiler/validator DLLs or a device without shader model 6.0 also select FXC
before any guest shaders compile. Startup logs and F8 diagnostics identify the
actual compiler. A failure to compile an individual DXC shader is reported;
it cannot silently introduce an FXC stage into a DXIL pipeline.

FXC retains the existing `shadercache/` files. DXC uses the child directory
`shadercache/dxc-1.8.2502-sm60-v1/` for bytecode, bundles, shader packs,
pipeline blobs, recipes, indexes and learned profiles. An explicit
`RSX_SHADER_CACHE=PATH` selects the parent, and `RSX_SHADER_CACHE=0` disables
both disk caches. The environment value itself is never rewritten. Old caches
are retained; a first DXC visit must learn/compile its own shaders and pipelines.

The guest stages use HLSL 2018 with IEEE strictness (`-Gis`), preserving the
output NaN guards. Literal texture/sampler arrays are rewritten to separate
register declarations, matching the renderer's sparse root signatures. The
port's own paired helper shaders continue using FXC.

Offline preparation selects its compiler explicitly:

```powershell
.venv\Scripts\python scripts/precompile_shaders.py --compiler dxc --scope executable
```

Existing `--assets`, `--compile-only`, `--limit` and cache options still apply.
The tool checks the installed compiler pair, uses the matching namespace, and
reports a device fallback instead of claiming a DXC preparation succeeded.

## Validation and remaining differences

The production compiler regression is
`.venv\Scripts\python scripts/test/shader_compiler.py`. It extracts the actual
backend implementation and checks concurrent initialization/compilation,
signed DXIL, repeatable output, sparse resource bindings on WARP, compilation
errors, explicit FXC, invalid selection and missing DLL fallbacks. Settings,
bundle and asynchronous pipeline regressions also cover the integration.

Live cold and warm GT5 High Speed Ring runs render the road, cars, crowd, HUD
and rear-view image. Startup warmup prepared 859 recorded DXC recipes with
zero rejections. These are short rendering checks, not full-race acceptance or
performance comparisons; the intermittent freeze from GT5 Phase 32 remains open.

`scripts/test/compiler_parity.cpp` is an optional GPU diagnostic adopted from
Prologue. Dump local translated sources with
`RSX_SHADER_SRC_DUMP=scratch/dxc/corpus` (up to 1,000 attempts / 64 MiB) and
build the harness using the command in its header. **Never commit that corpus.**

On the RTX 4060, two randomized trials of the first 100 captured GT5 sources
executed 20 vertex and 75 pixel shaders; five CPU-fetch vertex shaders were
unsupported by the harness. All supported sources compiled. Ten shaders were
flagged: four vertex shaders had non-finite differences, and six pixel shaders
had values beyond the diagnostic's 1/1024 tolerance. There were no discard
coverage differences. Smooth textures reduced the flagged pixel count to four.
An FXC-vs-FXC optimization-level control produced zero output differences.
These synthetic inputs do not establish a visible game defect, but they also
do not establish parity. DXC therefore remains an explicit option while those
cases are investigated.
