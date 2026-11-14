"""Run a declared sequence of private SPU comparisons, aborting failed cases.

Plan JSON: [{"tag":...,"case":"HSR","variant":"helpers","settle":60},...].
Games remain sequential and are owned by spu_live.ps1/gt5boot.ps1. Never
launches a compiler. Each run records its private executable's SHA256.
"""
import argparse
import json
from pathlib import Path
import subprocess

ROOT=Path(__file__).resolve().parents[2]


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('plan',type=Path)
    a=ap.parse_args()
    plan=json.loads(a.plan.read_text())
    folder=ROOT/'scratch/spu-codegen/live'
    folder.mkdir(exist_ok=True)
    for run in plan:
        tag=run['tag']
        if (folder/f'{tag}.json').exists():
            raise SystemExit(f'Run tag already exists: {tag}')
        command=['powershell','-NoProfile','-ExecutionPolicy','Bypass','-File',str(ROOT/'scripts/test/spu_live.ps1'),
                 '-Case',run.get('case','HSR'),'-Tag',tag,'-Exe',f"scratch/spu-codegen/{run['variant']}/player/gt5.exe",
                 '-Settle',str(run.get('settle',60)),'-SampleDelay',str(run.get('delay',20))]
        print('START',tag,run['variant'],run.get('case','HSR'),flush=True)
        with (folder/f'{tag}.runner.log').open('w') as f:
            result=subprocess.run(command,cwd=ROOT,stdout=f,stderr=subprocess.STDOUT)
        if result.returncode:
            raise SystemExit(f'{tag} failed; inspect {folder}/{tag}.runner.log')
        err=(ROOT/'scratch/pv'/f'{tag}.err').read_text(errors='replace')
        import re
        if re.search(r'input_nan [1-9]|input_inf [1-9]|nan=[1-9]|inf=[1-9]',err):
            raise SystemExit(f'{tag}: nonfinite audio; inspect stderr')
        metrics=ROOT/'scratch/pv'/f'{tag}.frames.json'
        print('DONE',tag,metrics.read_text() if metrics.exists() else 'acceptance run',flush=True)


if __name__=='__main__':
    main()
