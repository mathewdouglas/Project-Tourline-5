"""Check production fragment window coordinates on WARP, without game data."""
import ast
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
runtime = Path(os.environ.get('PS3RECOMP_DIR', root.parent / 'ps3recomp-tourline5'))
source = (runtime / 'libs/video/rsx_d3d12_backend.c').read_text(encoding='utf-8')
start = source.index('" float4 rsx_window_pos(')
end = source.index(';\n', start)
helper = ''.join(ast.literal_eval(s) for s in re.findall(r'"(?:[^"\\]|\\.)*"', source[start:end]))
# Exercise both the compiled path and the generated interpreter's coordinate code.
generator = (root / 'scripts/test/gen_fp_interp_hlsl.py').read_text(encoding='utf-8')
start = generator.index('    uint win = ')
end = generator.index('#if RSX_INTERP_LEGACY', start)
interp = 'float4 interp_window(float4 wpos) {' + generator[start:end] + 'return wpos;}'
work = Path(tempfile.mkdtemp(prefix='shader-window-', dir=root / 'scratch'))
commands = (runtime / 'libs/video/rsx_commands.c').read_text(encoding='utf-8')
start = commands.index('    if (method == NV4097_SET_SHADER_WINDOW) {')
end = commands.index('    if (method == NV4097_SET_SHADER_CONTROL)', start)
(work / 'decoder.inc').write_text('static int decode(rsx_state* state, u32 method, u32 data) {\n'
                                + commands[start:end] + 'return -1; }\n')
cases = []
for height in [720, 1080, 4095]:
    for bottom in [False, True]:
        for integer in [False, True]:
            window = height | (int(bottom) << 12) | (int(integer) << 16)
            # The highest expansion mask must survive window packing too.
            packed = float((window << 4) | 15)
            assert int(packed) & 15 == 15
            cases.append((packed, (960.375 - 0.5 * integer,
                                   (height - 270.5 if bottom else 270.5) - 0.5 * integer, 0.25, 0.5)))
hlsl = 'static float4 rsx_texexpand[4];\n' + helper + interp
hlsl += '\nRWStructuredBuffer<float4> result : register(u0);\n[numthreads(1,1,1)] void main() {\n'
for i, (packed, _) in enumerate(cases):
    hlsl += f'rsx_texexpand[0].x={packed}; result[{i}]=rsx_window_pos(float4(960.375,270.5,0.25,0.5));\n'
    hlsl += f'result[{i + 32}]=interp_window(float4(960.375,270.5,0.25,0.5));\n'
hlsl += '}\n'
(work / 'test.hlsl').write_text(hlsl, encoding='utf-8')
(work / 'build.cmd').write_text(f'''@echo off
set "PATH=C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer;%PATH%"
call "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
"C:\\Program Files\\LLVM\\bin\\clang-cl.exe" /nologo /O2 /MD /EHsc /D_CRT_SECURE_NO_WARNINGS /I"{work}" /I"{runtime}/include" /I"{runtime}/libs/video" /Fo"{work}/test.obj" /Fe"{work}/test.exe" "{root}/scripts/test/shader_window.cpp" /link d3d11.lib d3dcompiler.lib
''')
subprocess.run(['cmd', '/c', str(work / 'build.cmd')], check=True)
subprocess.run([str(work / 'test.exe'), str(work / 'test.hlsl'), str(work / 'result.bin')], check=True)
values = struct.unpack('256f', (work / 'result.bin').read_bytes())
for i, (_, expected) in enumerate(cases):
    for offset in [0, 32]:
        actual = values[(i + offset) * 4:(i + offset + 1) * 4]
        assert actual == expected, (i, offset, actual, expected)
print('PASS WARP: compiled/interpreted WPOS, both origins/centres, three heights, expansion mask intact')
