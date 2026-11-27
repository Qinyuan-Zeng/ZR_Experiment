"""Read-only replay of the two A0 sources and independent scoring of formal A."""
import csv
import json
from collections import defaultdict
from pathlib import Path
import numpy as np
from a0_replay import ROOT, POSITION, MODEL, longest_plateau, read_csv, digest

ID = "Z-AF-20261126-03"
result = ROOT / "results/formal_a" / ID
out = ROOT / "results/review" / ID
out.mkdir(parents=True, exist_ok=True)
manifest = ROOT / "manifest/formal_a" / ID
protocol = json.loads((manifest/"protocol.json").read_text(encoding="utf8"))
model = json.loads(MODEL.read_text(encoding="utf8"))
grid = np.array(protocol["grid"])
table = read_csv(result/"profiles.csv")
speeds = sorted({int(r["speed"]) for r in table})

def cube(field):
    lookup = {(int(r["round"]), int(r["speed"]), int(r["position_pulse"])): float(r[field]) for r in table}
    assert len(lookup) == len(table) == 5*17*49
    return np.array([[[lookup[i,v,x] for x in grid] for v in speeds] for i in range(1,6)])

def save(name, header, rows):
    with (out/name).open("w", encoding="utf-8-sig", newline="") as f:
        w = csv.writer(f)
        w.writerow(header.split())
        w.writerows(rows)

p,n = cube("positive_raw"),cube("negative_raw")
odd,even = (p-n)/2,(p+n)/2
assert np.allclose(odd,cube("half_diff_raw")) and np.allclose(even,cube("half_sum_raw"))
comparison = []
for root,session in [(POSITION,"Z-C_20261124-165139_02"),(ROOT,"20261126T072144345Z_18692")]:
    base = root/"manifest"
    meta = dict(l.split("=",1) for l in (base/(session+".txt")).read_text(encoding="utf-8-sig").splitlines() if "=" in l)
    raw = root/"data/raw"/(session+".csv")
    sources=json.loads((ROOT/"results"/(session+".stage3_A0")/"sources.json").read_text(encoding="utf-8-sig"))
    assert digest(raw) in sources["sha256"].values()
    samples = defaultdict(list)
    for row in read_csv(raw):
        if row["use"]=="development":
            samples[row["trial_id"]].append(row)
    branches = defaultdict(dict)
    for t in read_csv(base/(session+".trials.csv")):
        if t["use"]!="development": continue
        x,v,a,y=longest_plateau(samples[t["trial_id"]],t,model["scan_policy"],meta,model)
        full=np.arange(3500,22001,250)
        assert x[0]<=full[0] and x[-1]>=full[-1]
        branches[int(float(t["speed_mm_s"])*1000),int(t["group"])][int(t["direction"])]=np.array([
            np.interp(full,x,y).mean(),np.interp(grid,x,y).mean(),
            np.interp(grid,x,y-protocol["alpha"]*a).mean()])
    for v in sorted({v for v,g in branches}):
        sets=[b for (s,g),b in branches.items() if s==v]
        wide,crop,aligned=np.mean([(b[1]-b[-1])/2 for b in sets],axis=0)
        formal=odd[:,speeds.index(v)].mean()
        comparison.append([session,v,wide,crop,aligned,formal,wide-crop,crop-aligned,aligned-formal])
save("alignment.csv","session speed wide crop aligned formal grid_effect inertia_effect remaining",comparison)
spatial=[]
for j,v in enumerate(speeds):
    for label,array in [("half_diff",odd),("half_sum",even)]:
        z=array[:,j,:]
        centered=z-z.mean(axis=1,keepdims=True)
        dev=centered[:4].mean(axis=0)
        test=centered[4]
        for i,x in enumerate(grid):
            spatial.append([v,label,x,dev[i],test[i],np.corrcoef(dev,test)[0,1]])
save("space.csv","speed component position S01_centered S02_centered correlation",spatial)
models=json.loads((manifest/"models_frozen.json").read_text())["models"]
metrics=[]
for direction,y,vfield in [(1,p,"positive_velocity_pulse_s"),(-1,n,"negative_velocity_pulse_s")]:
    vel=np.abs(cube(vfield))/1000
    train=[j for j,v in enumerate(speeds) if v not in (4000,14000,32000)]
    train_v=vel[:4,train].mean(axis=2).ravel()
    train_y=y[:4,train].mean(axis=2).ravel()
    assert np.allclose(np.polyfit(train_v,train_y,1)[::-1],models["F1"][str(direction)],rtol=1e-10)
    assert np.isclose(train_y.mean(),models["F0"][str(direction)][0])
    for j,v in enumerate(speeds):
        for split,idx in [("speed_validation" if v in (4000,14000,32000) else "development",slice(0,4)),("S02",slice(4,5))]:
            for name,branches in models.items():
                intercept,slope=branches[str(direction)]
                e=y[idx,j]-intercept-slope*vel[idx,j]
                metrics.append([v,split,direction,name,np.sqrt(np.mean(e**2)),np.percentile(abs(e),95),e.mean()])
save("errors.csv","speed split direction model rmse p95 bias",metrics)
for reported in read_csv(result/"model_metrics.csv"):
    split="S02" if reported["split"]=="independent_session" else reported["split"]
    group=[m for m in metrics if m[1:4]==[split,int(reported["direction"]),reported["model"]]]
    assert np.isclose(np.sqrt(np.mean([m[4]**2 for m in group])),float(reported["rmse_raw"]))
for row in comparison: print(row[:2],np.round(row[2:],4))
for label in ("half_diff","half_sum"):
    subset=np.array([[r[3],r[4],r[5]] for r in spatial if r[1]==label]).reshape(17,49,3)
    print(label,'correlation min median max',np.percentile(subset[:,0,2],[0,50,100]),'test spatial RMS',np.sqrt(np.mean(subset[:,:,1]**2)),'after S01 pattern',np.sqrt(np.mean((subset[:,:,1]-subset[:,:,0])**2)))
for v in speeds:
    print(v,'S02 F0,F1 + then -',np.round([r[4] for r in metrics if r[0]==v and r[1]=="S02"],2))
