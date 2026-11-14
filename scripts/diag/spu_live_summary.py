"""Summarize completed declared SPU measurements, including per-frame CPU."""
import argparse
import json
from pathlib import Path
import statistics

ROOT=Path(__file__).resolve().parents[2]


def summarize(plan):
    rows=[]
    for run in plan:
        tag=run['tag'];metrics=ROOT/'scratch/pv'/f'{tag}.frames.json'
        if not metrics.exists(): continue
        frame=json.loads(metrics.read_text(encoding='utf-8-sig'))
        cpu=json.loads((ROOT/'scratch/pv'/f'{tag}.cpu.json').read_text(encoding='utf-8-sig'))
        sec=cpu['seconds'];n=frame['frames']
        row=dict(tag=tag,case=run.get('case','HSR'),variant=run['variant'],fps=frame['fps'],
                 p99_ms=frame['p99_ms'],late_percent=frame['late_percent'],walk_ms=frame['walk_ms'],
                 cpu_ms_per_frame=1000*cpu['process_cpu_seconds']/n)
        for name,label in [('spurs-jc','job'),('spurs-lane','lane')]:
            total=sum(t['CpuSeconds'] for t in cpu['threads'] if t['Name']==name)
            row[label+'_cores']=total/sec
            row[label+'_ms_per_frame']=1000*total/n
        rows.append(row)
    groups=[]
    for case,variant in dict.fromkeys((r['case'],r['variant']) for r in rows):
        selected=[r for r in rows if r['case']==case and r['variant']==variant]
        group=dict(case=case,variant=variant,runs=len(selected))
        for key in ('fps','p99_ms','late_percent','walk_ms','cpu_ms_per_frame','job_cores','lane_cores','job_ms_per_frame','lane_ms_per_frame'):
            group[key]=statistics.median(r[key] for r in selected)
        groups.append(group)
    return dict(runs=rows,groups=groups)


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('plan',type=Path)
    ap.add_argument('--output',type=Path,required=True)
    ap.add_argument('--html',type=Path)
    a=ap.parse_args()
    result=summarize(json.loads(a.plan.read_text()))
    a.output.write_text(json.dumps(result,indent=2))
    if a.html:
        page='''<!doctype html><html><head><meta charset="utf-8"><style>
body{font-family:var(--font-sans);color:var(--foreground);margin:0} .controls{display:flex;gap:12px;flex-wrap:wrap;align-items:center;margin:8px 0 12px}select{font:inherit;padding:6px;background:var(--card);color:var(--foreground);border:1px solid var(--border);border-radius:var(--radius)}p{font-size:13px;color:var(--muted-foreground);line-height:1.5;margin:8px 0}svg{width:100%;height:auto;display:block}svg text{fill:var(--foreground);font-family:var(--font-sans)}.grid{stroke:var(--border)}
</style></head><body><div class="controls"><label>Scene <select id="scene"></select></label><label>Measure <select id="metric"><option value="fps">FPS · higher is better</option><option value="cpu_ms_per_frame">Total CPU per frame · lower is better</option><option value="job_cores">SPURS job CPU · lower is better</option></select></label></div><svg id="plot" viewBox="0 0 700 420" role="img" aria-label="SPU performance comparisons"></svg><p>Dots are individual runs; diamonds are medians. Dashed line: baseline median. 720p, 2× MSAA, DXC, VSync off; alternating runs, no compiler or concurrent game. Short-route measurements, not a full playthrough.</p><script>
const data=DATA;
const labels={'cache-only':'Baseline · 16 slots','helpers':'Integer helpers','shuffle-or':'Shuffle OR','lazy2':'Lazy locals','sysv':'System V','lazy-sysv':'Locals + System V','lazy-ls':'Full locals + LS base','lazy-ls-sysv':'Full locals + LS + ABI','full-reference':'Full package'};
const scene=document.getElementById('scene'),metric=document.getElementById('metric'),plot=document.getElementById('plot');
for(const c of [...new Set(data.groups.map(g=>g.case))])scene.add(new Option(c,c));
function draw(){let key=metric.value,groups=data.groups.filter(g=>g.case===scene.value),runs=data.runs.filter(r=>r.case===scene.value);let values=runs.map(r=>r[key]);let lo=Math.min(...values),hi=Math.max(...values),pad=Math.max((hi-lo)*.3,key==='fps'?1:.02);lo=Math.max(0,lo-pad);hi+=pad;let x=v=>190+(v-lo)/(hi-lo)*440;let h=65+groups.length*48;plot.setAttribute('viewBox',`0 0 700 ${h}`);let parts=[];for(let t=0;t<=5;t++){let value=lo+(hi-lo)*t/5;parts.push(`<line class="grid" x1="${x(value)}" x2="${x(value)}" y1="25" y2="${h-30}"/><text x="${x(value)}" y="${h-10}" text-anchor="middle" font-size="11">${value.toFixed(key==='job_cores'?2:1)}</text>`)}let baseline=groups.find(g=>g.variant==='cache-only');if(baseline)parts.push(`<line x1="${x(baseline[key])}" x2="${x(baseline[key])}" y1="20" y2="${h-30}" stroke="var(--muted-foreground)" stroke-dasharray="4 4"/>`);groups.forEach((g,i)=>{let y=45+i*48,c=`var(--chart-${i%6+1})`;parts.push(`<text x="0" y="${y+4}" font-size="13">${labels[g.variant]||g.variant}</text>`);runs.filter(r=>r.variant===g.variant).forEach((r,j)=>parts.push(`<circle cx="${x(r[key])}" cy="${y+(j%2?6:-6)}" r="4" fill="${c}" opacity=".65"><title>${r.tag}: ${r[key].toFixed(3)}</title></circle>`));let m=x(g[key]);parts.push(`<path d="M${m},${y-6} l6,6 -6,6 -6,-6 Z" fill="${c}"/><text x="648" y="${y+4}" font-size="13">${g[key].toFixed(key==='job_cores'?3:2)}</text>`)});plot.innerHTML=parts.join('')}
scene.onchange=metric.onchange=draw;draw();
</script></body></html>'''
        a.html.write_text(page.replace('DATA',json.dumps(result)),encoding='utf-8')
    for r in result['groups']:
        print(f"{r['case']:7} {r['variant']:12} n={r['runs']} fps={r['fps']:.3f} p99={r['p99_ms']:.2f} late={r['late_percent']:.2f}% job={r['job_cores']:.3f} cores CPU/frame={r['cpu_ms_per_frame']:.2f} ms")


if __name__=='__main__': main()
