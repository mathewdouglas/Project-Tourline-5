"""Replay the actual coefficient constructor using synthetic inputs; no game/device run."""
from pathlib import Path
import json
import os
import re
import subprocess
import sys
import tempfile

root=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(root/'scripts'))
import spu_native_patch

# The call/drain/image-restore line must survive stripping, and an unexpected
# call count must refuse patching rather than silently dropping coverage.
line=('ctx->gpr[0]=spu_link(0x123); { int32_t _si = (int32_t)ctx->image_id; '
      'ctx->host_depth++; gt5p_fixture_spu_func_000004E8(ctx); '
      'spu_drain_call(ctx, 0x123); ctx->host_depth--; spu_img_restore(ctx, _si); }')
fixture=['before',line,'other_call(ctx);',line,'after']
wrapped,missing=spu_native_patch.wrap_audio_round_calls(fixture,'fixture',[(0x4E8,2)])
assert not missing
restored=[s for s in wrapped if spu_native_patch.MARK not in s]
assert restored==fixture
assert spu_native_patch.wrap_audio_round_calls(restored,'fixture',[(0x4E8,2)])[0]==wrapped
assert spu_native_patch.wrap_audio_round_calls(fixture,'fixture',[(0x4E8,3)])[1]
meta=json.loads((root/'config/targets.json').read_text())['BCUS98114-v104']
assert all(image in meta['audio_extended_zero_images'] for image,_,_ in meta['spu_audio_round_zero_calls'])

runtime=Path(os.environ.get('PS3RECOMP_DIR',root.parent/'ps3recomp-tourline5'))
folder=Path(tempfile.mkdtemp(prefix='audio-coefficients-',dir=root/'scratch'))
image=root/'generated/spu_lifted/j9804ABA8'
src=(image/'spu_recomp.c').read_text()
blocks=[]
for m in re.finditer(r'void gt5p_j9804ABA8_spu_func_([0-9A-F]+)\(spu_context\* ctx\) \{.*?\n}',src,re.S):
    if 0x4E8<=int(m[1],16)<0x818: blocks.append(m[0])
assert blocks and '000004E8' in blocks[0]
(folder/'spu_recomp.h').write_bytes((image/'spu_recomp.h').read_bytes())
(folder/'kernel.c').write_text('#include "gt5p_spu_audio_math.h"\n#include "spu_recomp.h"\n'+'\n'.join(blocks))
vc=('@echo off\nset "PATH=C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer;%PATH%"\n'
    'call "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\n')
for option in (0,1):
    # The coefficient scope remains available with the independent extended-
    # value fix disabled; that build option must remain a usable rollback.
    cmd=(f'"C:/Program Files/LLVM/bin/clang-cl.exe" /nologo /O2 /MD /std:c17 /D_CRT_SECURE_NO_WARNINGS '
         f'/DSPU_LS_NO_PROBES /DGT5P_AUDIO_EXTENDED_ZERO={option} -msse4.1 '
         f'/I"{runtime}/runtime/spu" /I"{root}/src" /I"{folder}" '
         f'/Fo"{folder}/" /Fe"{folder}/test{option}.exe" "{root}/scripts/test/audio_coefficients.c" "{folder}/kernel.c"\n')
    batch=folder/f'build{option}.cmd';batch.write_text(vc+cmd)
    subprocess.run(['cmd','/c',str(batch)],check=True)
    env=os.environ.copy();env.pop('GT5P_AUDIO_ROUND_ZERO',None)
    subprocess.run([str(folder/f'test{option}.exe')],env=env,check=True,timeout=30)
    env['GT5P_AUDIO_ROUND_ZERO']='0'
    subprocess.run([str(folder/f'test{option}.exe'),'rollback'],env=env,check=True,timeout=30)
