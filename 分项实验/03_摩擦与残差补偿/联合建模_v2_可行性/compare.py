"""Round 2. Develop on repeat folds, then score the sealed candidate suite once."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import sys

import os
os.environ["OPENBLAS_NUM_THREADS"] = "1"
os.environ["OMP_NUM_THREADS"] = "1"

import numpy as np
import pandas as pd

OUT = Path(__file__).resolve().parent
V1 = OUT.parent/"联合建模_v1"
sys.path.insert(0,str(V1))
import train_joint as base

DATA = V1/"joint_samples.csv.gz"
MODELS = {
    "shared_v1": ("shared", []),
    "shared_jerk": ("shared", ["jerk"]),
    "shared_arrival": ("shared", ["arrival_speed","travel"]),
    "shared_history": ("shared", ["slow_velocity","slow_still"]),
    "shared_all": ("shared", ["jerk","arrival_speed","travel","slow_velocity","slow_still"]),
    "hard_experts": ("hard", []),
    "soft_experts": ("soft", []),
    "semi_hold": ("semi", []),
}
SEEDS = [426,427,428]
DYNAMIC = ["accelerate","cruise","brake"]
PHASES = ["ALL","loading",*base.PHASES,"hold_low_speed"]
SNAPSHOT = ["models/M2.npz","data_config.json","training_config.json","evaluation.csv",
            "evaluation_by_trial.csv","evaluation_subgroups.csv","paired_regression_check.csv",
            "seed_sensitivity.csv","splits.csv","internal_predictions.csv.gz","prepare_joint.py","train_joint.py"]
CONFIG = dict(repeat_folds=[1,2], seeds=SEEDS, menu=MODELS, training=base.CONFIG,
    baseline=base.FEATURES, expert_widths=[16,10], expert_budget="12 updates per branch with 2 MLP branches; 24 for a single branch",
    hold_gate="smoothstep time_since_stop from .005 to .080 s times exp(-(abs(v)/500)^2); true phases used only in weighting/evaluation",
    jerk="backward 20 ms difference of causal acceleration clipped to ±1e8 pulse/s^3",
    arrival="peak measured |v| / accumulated absolute encoder displacement in most recent observed motion",
    half_mechanistic="ridge 0.001; fixed 0.2,1,3 s decay bases with directional, position and arrival-speed terms",
    selection="average within-fold/seed loading and HOLD RMSE ratios to shared_v1; must improve both aggregate ratios, with average acceleration/brake ratios <=1.05",
    internal="full sealed suite evaluated together; no selection on internal",E_used=False)


def sha(path):
    with path.open("rb") as f:
        return hashlib.file_digest(f,"sha256").hexdigest()


def save(name,value):
    (OUT/name).write_text(json.dumps(value,indent=2,ensure_ascii=False),encoding="utf-8")


def csv(name,value):
    value.to_csv(OUT/name,index=False,encoding="utf-8-sig")


def snapshot():
    snap = OUT/"M2基准"
    hashes = {}
    for name in SNAPSHOT:
        source = V1/name
        target = snap/name
        target.parent.mkdir(parents=True,exist_ok=True)
        if not target.exists():
            shutil.copy2(source,target)
        assert sha(target)==sha(source), name
        hashes[name] = sha(source)
    hashes["joint_samples.csv.gz"] = sha(DATA)
    assert hashes["joint_samples.csv.gz"]==json.loads((V1/"training_config.json").read_text())["dataset_sha256"]
    save("baseline_hashes.json",hashes)
    return hashes


def features(x,v,a,stop):
    n = len(x)
    z = np.zeros((n,5))
    peak = distance = 0.
    slow_v, slow_still = 0.,1.
    moving = False
    for i in range(n):
        if not np.isfinite(v[i]):
            z[i] = (0,peak,distance,slow_v,slow_still)
            continue
        if stop[i]>0:
            moving = False
        elif abs(v[i])>=600 and not moving:
            peak,distance = 0.,0.
            moving = True
        if moving:
            peak = max(peak,abs(v[i]))
            distance += abs(x[i]-x[i-1]) if i else 0.
        slow_v += (1-np.exp(-.005/.35))*(v[i]-slow_v)
        slow_still += (1-np.exp(-.005/.35))*(np.exp(-(v[i]/200)**2)-slow_still)
        jerk = np.clip((a[i]-a[i-4])/.020,-1e8,1e8) if i>=4 and np.isfinite(a[i-4]) else 0.
        z[i] = (jerk,peak,distance,slow_v,slow_still)
    return z


def dataset():
    t = pd.read_csv(DATA,low_memory=False)
    assert t.groupby("trace",sort=False).ngroup().is_monotonic_increasing
    blocks = []
    for _,g in t.groupby("trace",sort=False):
        assert g.frame.is_monotonic_increasing
        inp = g[["x_pulse","v_pulse_s","a_pulse_s2","time_since_stop"]].to_numpy().T
        z = features(*inp)
        blocks.append(z)
    t[["jerk","arrival_speed","travel","slow_velocity","slow_still"]] = np.vstack(blocks)
    x = np.array([0,1,3,2,2,2,1,1,1,1,1,1],dtype=float)
    v = np.array([0,700,800,900,800,900,800,0,0,0,0,0],dtype=float)
    a,stop = np.zeros(len(x)),np.r_[np.zeros(8),.005,.010,.015,.020]
    full = features(x,v,a,stop)
    assert np.allclose(full[:8],features(x[:8],v[:8],a[:8],stop[:8]))
    assert t.groupby("unit").split.nunique().eq(1).all()
    arrival_nominal = np.where((t.cohort=="C")&(t.segment=="dwell"),
                               np.where(t.history.eq("direct"),2,10),t.speed)
    t["low_hold"] = (t.phase=="hold")&(arrival_nominal<=5)
    return t


def gate(t,mode):
    if mode=="hard":
        return ((t.time_since_stop>=.015)&(t.v_pulse_s.abs()<=400)).to_numpy(float)
    z = np.clip((t.time_since_stop.to_numpy()-.005)/.075,0,1)
    return z*z*(3-2*z)*np.exp(-(t.v_pulse_s.to_numpy()/500)**2)


def physical(t):
    direction = t.arrival_memory.to_numpy()
    x = (t.x_pulse.to_numpy()-16000)/6000
    age = t.time_since_stop.to_numpy()
    speed = np.log1p(t.arrival_speed.to_numpy()/10000)
    e = [np.exp(-age/tau) for tau in (.2,1,3)]
    return np.column_stack([np.ones(len(t)),x,direction,direction*x,direction*speed,
                           *(direction*a for a in e),*(direction*speed*a for a in e),e[1]])


def train_mlp(x,y,w,seed,expert=False,two_branches=False):
    original = base.CONFIG.copy()
    try:
        if expert:
            base.CONFIG["widths"] = [16,10]
        if two_branches:
            base.CONFIG["steps_per_epoch"] = 12
        return base.train(x,y,w/w.sum(),seed)
    finally:
        base.CONFIG.clear()
        base.CONFIG.update(original)


def prediction(name,train,test,seed):
    mode,more = MODELS[name]
    cols = base.FEATURES+more
    y = train.label_raw.to_numpy()
    w = base.weights(train)
    x,xt = train[cols].to_numpy(),test[cols].to_numpy()
    if mode=="shared":
        net = train_mlp(x,y,w,seed)
        return test.m1_raw.to_numpy()+base.predict(net,xt)
    g,gt = gate(train,mode),gate(test,mode)
    dyn = train_mlp(x,y,w*(1-g),seed,expert=True,two_branches=mode!="semi")
    dp = base.predict(dyn,xt)
    if mode=="semi":
        a = physical(train)
        ww = w*g
        ww /= ww.sum()
        coef = np.linalg.solve(a.T@(ww[:,None]*a)+.001*np.diag([0]+[1]*(a.shape[1]-1)),a.T@(ww*y))
        hp = physical(test)@coef
    else:
        hold = train_mlp(x,y,w*g,seed+1000,expert=True,two_branches=True)
        hp = base.predict(hold,xt)
    return test.m1_raw.to_numpy()+dp*(1-gt)+hp*gt


def score(name,seed,fold,t,yp):
    error = t.current_raw.to_numpy()-yp
    results = []
    for cohort in ("ALL","B1","C","D"):
        scope = np.ones(len(t),bool) if cohort=="ALL" else t.cohort.eq(cohort).to_numpy()
        for phase in PHASES:
            if phase=="ALL":
                mask=scope
            elif phase=="loading":
                mask=scope & t.phase.isin(DYNAMIC).to_numpy()
            elif phase=="hold_low_speed":
                mask=scope & t.low_hold.to_numpy()
            else:
                mask=scope & t.phase.eq(phase).to_numpy()
            results.append(dict(model=name,seed=seed,fold=fold,cohort=cohort,phase=phase,
                                **base.evaluate(t.loc[mask],error[mask])))
    return results


def run(train,test,fold):
    assert not set(train.unit)&set(test.unit)
    assert set(train.split)=={"dev"}
    out = score("M1",426,fold,test,test.m1_raw.to_numpy())
    preds = {}
    for name in MODELS:
        for seed in SEEDS:
            yp = prediction(name,train,test,seed)
            out.extend(score(name,seed,fold,test,yp))
            if seed==426:
                preds[name] = yp
        print(fold,name,flush=True)
    return pd.DataFrame(out),preds


def preferences(dev):
    e = dev.loc[(dev.cohort=="ALL") & dev.phase.isin(["loading","hold","accelerate","brake"]) &
                (dev.model!="M1")].pivot(index=["model","fold","seed"],columns="phase",values="rmse")
    ratio = e.reset_index().merge(
        e.loc["shared_v1"].add_suffix("_base").reset_index(),on=["fold","seed"],validate="many_to_one")
    columns=["loading","hold","accelerate","brake"]
    for k in columns:
        ratio[k] /= ratio[k+"_base"]
    stats=ratio.groupby("model")[columns].mean()
    stats["score"]=(stats.loading+stats.hold)/2
    stats["passes"]=(stats.loading<1)&(stats.hold<1)&(stats.accelerate<=1.05)&(stats.brake<=1.05)
    stats=stats.sort_values(["passes","score"],ascending=[False,True])
    csv("development_selection.csv",stats.reset_index())
    return next(iter(stats.index[stats.passes]),"shared_v1")


def develop(t):
    hashes=snapshot()
    save("candidate_config.json",CONFIG)
    dev=t.loc[t.split.eq("dev")&t.candidate]
    rows=[]
    for r in CONFIG["repeat_folds"]:
        hold=dev["round"].eq(r)
        result,_=run(dev.loc[~hold],dev.loc[hold],f"dev_R{r}")
        rows.append(result)
    results=pd.concat(rows,ignore_index=True)
    csv("development_metrics.csv",results)
    preferred=preferences(results)
    seal=dict(preferred_on_dev=preferred,config=sha(OUT/"candidate_config.json"),
              script=sha(Path(__file__)),dataset=hashes["joint_samples.csv.gz"],dev_metrics=sha(OUT/"development_metrics.csv"))
    save("selection_seal.json",seal)
    print("Selected using dev only:",preferred,flush=True)


def internal(t):
    seal=json.loads((OUT/"selection_seal.json").read_text(encoding="utf-8"))
    assert seal["config"]==sha(OUT/"candidate_config.json") and seal["script"]==sha(Path(__file__))
    assert seal["dev_metrics"]==sha(OUT/"development_metrics.csv") and seal["dataset"]==sha(DATA)
    dev=t.loc[t.candidate&t.split.eq("dev")]
    test=t.loc[t.candidate&t.split.eq("internal")]
    results,preds=run(dev,test,"internal")
    frozen=OUT/"M2基准/models/M2.npz"
    with np.load(frozen,allow_pickle=False) as f:
        x=(test[f["features"].tolist()].to_numpy()-f["x_mean"])/f["x_scale"]
        old=base.forward(x,[f[k] for k in ("W1","b1","W2","b2","W3","b3")])[2][:,0]*f["y_scale"]+f["y_mean"]
    old+=test.m1_raw.to_numpy()
    assert np.max(np.abs(old-preds["shared_v1"]))<.01
    original=pd.read_csv(V1/"internal_predictions.csv.gz")[["trace","frame","M2"]]
    actual=test[["trace","frame"]].assign(frozen=old).merge(original,on=["trace","frame"],validate="one_to_one")
    assert len(actual)==len(test) and np.max(np.abs(actual.frozen-actual.M2))<1e-6
    results=pd.concat([results,pd.DataFrame(score("M2_frozen",426,"internal",test,old))],ignore_index=True)
    csv("internal_metrics.csv",results)
    out=test[["unit","trace","cohort","phase","frame","current_raw"]].copy()
    out["M2_frozen"]=old
    for name,v in preds.items():
        out[name]=v
    out.to_csv(OUT/"internal_predictions.csv.gz",compression=dict(method="gzip",compresslevel=3,mtime=0),index=False)
    assert all(sha(V1/n)==v for n,v in json.loads((OUT/"baseline_hashes.json").read_text()).items())


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode",choices=["develop","internal"])
    args=parser.parse_args()
    if args.mode=="develop" and (OUT/"internal_metrics.csv").exists():
        raise SystemExit("Existing internal score is sealed. Keep this run and use a new directory for another study.")
    t=dataset()
    if args.mode=="develop":
        develop(t)
    else:
        internal(t)


if __name__=="__main__":
    main()
