"""Check CPU/OS fallback, image identity, preflight and actual registry replacement."""
import os
import json
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
runtime=Path(os.environ.get('PS3RECOMP_DIR',root.parent/'ps3recomp-tourline5'))
(root/'scratch').mkdir(exist_ok=True)
folder=Path(tempfile.mkdtemp(prefix='audio-native-',dir=root/'scratch'))
s=(runtime/'runtime/spu/spu_channels.c').read_text(encoding='utf-8')
a=s.index('typedef void (*spu_fn)');b=s.index('/* Report a CROSS-IMAGE match:',a)
(folder/'registry.inc').write_text(s[a:b],encoding='utf-8')
targets=json.loads((root/'config/targets.json').read_text())
for target in ('BCUS98158','BCUS98114-v104'):
 entries=targets[target]['audio_native_images']
 manifest=''.join(f'AUDIO_IMAGE({name}, {fp}ULL, {image})\n' for name,fp,image in entries)
 for name,fp,image,ea,span in targets[target].get('spu_fma_images',[]):
  manifest+=f'SPU_IMAGE({name}, {fp}ULL, {image}, {ea}, {span})\n'
 (folder/'gt5p_audio_images.inc').write_text(manifest)
 batch=folder/'build.cmd'
 batch.write_text('@echo off\nset "PATH=C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer;%PATH%"\ncall "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\n'
  + '"C:\\Program Files\\LLVM\\bin\\clang-cl.exe" /nologo /O2 /MD /EHsc /D_CRT_SECURE_NO_WARNINGS /DGT5P_HAVE_AUDIO_NATIVE=1 '
  + ('/DTEST_GT5 ' if target=='BCUS98114-v104' else '')
  + f'/I"{runtime}/runtime/spu" /I"{folder}" /Fo"{folder}/" /Fe"{folder}/dispatch.exe" "{root}/scripts/test/audio_native.cpp"\n'
  + 'if errorlevel 1 exit /b 1\n'
  + '"C:\\Program Files\\LLVM\\bin\\clang-cl.exe" /nologo /O2 /MD /D_CRT_SECURE_NO_WARNINGS '
  + f'/I"{folder}" /Fo"{folder}/" /Fe"{folder}/registry.exe" "{root}/scripts/test/audio_registry.c"\n',encoding='ascii')
 subprocess.run(['cmd','/c',str(batch)],check=True,cwd=root)
 subprocess.run([str(folder/'registry.exe')],check=True,timeout=10)
 for case in ('off','default','cpu','os','missing','identity','absent','on','body_mismatch','body_missing','render_only'):
  if case=='render_only' and target!='BCUS98114-v104': continue
  for render in ('0','1','b0196FF40','b01697100,b016D2700') if target=='BCUS98114-v104' and case not in ('off','missing') else ('0',):
   if case=='render_only': render='1'
   env=os.environ.copy();env.pop('GT5P_AUDIO_NATIVE',None)
   env['GT5P_SPU_FMA']=render
   if case!='default':env['GT5P_AUDIO_NATIVE']='0' if case in ('off','render_only') else '1'
   subprocess.run([str(folder/'dispatch.exe'),case],env=env,check=True,timeout=10)
 print(target+': dispatch checks passed')
