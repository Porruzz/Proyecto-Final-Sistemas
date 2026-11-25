import sys,pathlib,csv,json
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[1]))
from simulator.api import *
from applications.apps import regression,image
rows=[]
for n in (3,4,5,6):
 for strategy in range(4):
  for loss in (0,10,20,30):
   for seed in (1,2,3):
    reset(n,seed,loss,strategy); completed=0; times=[]; errors=0
    for i in range(12):
     try:
      r=Node(i%n).run(1,b'\x05');completed+=r['status']==0;times.append(r['latency_ms'])
     except (RuntimeError,TimeoutError):errors+=1
    s=snapshot();rows.append(dict(nodes=n,strategy=strategy,loss=loss,seed=seed,submitted=12,completed=completed,errors=errors,mean_task_ms=sum(times)/len(times) if times else '',frames=s['sent'],dropped=s['dropped'],reassigned=sum(x['reassigned'] for x in s['nodes'])))
p=ROOT/'results';p.mkdir(exist_ok=True)
with (p/'benchmark.csv').open('w',newline='') as f:w=csv.DictWriter(f,fieldnames=rows[0]);w.writeheader();w.writerows(rows)
(p/'benchmark.json').write_text(json.dumps(rows,indent=2))
reset();assert regression([(i,2*i+3) for i in range(60)])['mse']==0
assert image(bytes([255,0,0]*60))[0]==bytes([76]*60)
reset();h=Node().submit(1,bytes([100]));lib.advance(100);lib.toggle(2,0);r=Node().result(h);assert r['status']==0;r['data']=r['data'].hex();(p/'failure.json').write_text(json.dumps(r,indent=2));print(len(rows),'escenarios; aplicaciones y recuperación verificadas',r)
