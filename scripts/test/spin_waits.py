"""Check the target's real private wait loops and the reversible post-lift transformation."""
from pathlib import Path
import re, sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import spin_patch
import target
assert target.NAME=='BCUS98114-v104', 'select GT5P_TARGET=BCUS98114-v104'
for func_s,label_s in target.TARGET['spin_waits']:
 func,label=f'{int(func_s,16):08X}',f'{int(label_s,16):08X}'
 path,text,start,end=spin_patch.find_function(func)
 body=spin_patch.unpatch_body(text[start:end])
 patched,n=spin_patch.patch_body(body,func,label)
 assert n==1, (func,'unexpected wait back-edge count',n)
 assert spin_patch.unpatch_body(patched)==body
 loop=body[body.index('loc_'+label+':'):body.index('goto loc_'+label+';')]
 # No guest memory stores, atomics or time/service calls on the waiting path.
 assert not re.search(r'vm_write|ppu_stwcx|func_00C32230|ps3_indirect_call',loop),func
 if func!='00D1B930': assert 'func_00D1AB10(ctx)' in loop,func
 else: assert 'PPU_FENCE(acq_rel)' in loop and loop.count('vm_read16')==7
 print(f'{func} -> {label}: one read-only progress-wait back edge; patch/restore exact')
