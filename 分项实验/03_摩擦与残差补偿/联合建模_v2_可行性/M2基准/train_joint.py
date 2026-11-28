"""Train fixed, small residual MLPs using only development samples."""
from pathlib import Path
import json
import os
os.environ["OPENBLAS_NUM_THREADS"] = "1"
os.environ["OMP_NUM_THREADS"] = "1"

import numpy as np
import pandas as pd
from prepare_joint import OUT, FEATURES, csv, sha

MODELS = {
    "M2": FEATURES,
    "M2_no_direction": [k for k in FEATURES if k!="arrival_memory"],
    "M2_no_time": [k for k in FEATURES if k!="time_since_stop"],
    "M2_no_both": FEATURES[:5],
}
SEEDS = [426, 427, 428]
PHASES = ["startup","accelerate","cruise","brake","near_zero","hold"]
CONFIG = dict(widths=[24,16], activation="tanh", epochs=100, steps_per_epoch=24, batch_size=256,
              optimizer="Adam", learning_rate=.002, l2=.0001, huber_delta_raw=25,
              seeds=SEEDS, saved_seed=SEEDS[0], dtype="float64", E_used=False,
              schedule="fixed before internal scoring; score all seeds; no selection on internal",
              weight="equal populated cohort-phase strata, equal complete units within each, equal rows within unit")


def weights(frame):
    by_unit = frame.groupby(["cohort","phase","unit"]).unit.transform("size").to_numpy()
    unit_count = frame.groupby(["cohort","phase"]).unit.transform("nunique").to_numpy()
    w = 1/(by_unit*unit_count)
    return w/w.sum()


def normalize(x,w):
    mean = w@x
    scale = np.sqrt(w@(x-mean)**2)
    return mean,np.where(scale<1e-9,1.,scale)


def forward(x,p):
    w1,b1,w2,b2,w3,b3 = p
    a = np.tanh(x@w1+b1)
    b = np.tanh(a@w2+b2)
    return a,b,b@w3+b3


def train(x,y,weight,seed):
    rng = np.random.default_rng(seed)
    widths = [x.shape[1],*CONFIG["widths"],1]
    p = []
    for a,b in zip(widths,widths[1:]):
        limit = np.sqrt(6/(a+b))
        p.extend((rng.uniform(-limit,limit,(a,b)),np.zeros(b)))
    m = [np.zeros_like(a) for a in p]
    vv = [np.zeros_like(a) for a in p]
    center,scale = normalize(x,weight)
    y0,ys = float(weight@y),float(np.sqrt(weight@(y-weight@y)**2))
    assert ys>0
    x,y = (x-center)/scale, ((y-y0)/ys)[:,None]
    delta = CONFIG["huber_delta_raw"]/ys
    cdf = np.cumsum(weight)
    cdf[-1] = 1.
    trace = []
    step = 0
    for epoch in range(1,CONFIG["epochs"]+1):
        losses = []
        for _ in range(CONFIG["steps_per_epoch"]):
            ix = np.searchsorted(cdf,rng.random(CONFIG["batch_size"]))
            xx, yy = x[ix], y[ix]
            aa,bb,yp = forward(xx,p)
            e = yp-yy
            ae = np.abs(e)
            losses.append(float(np.mean(np.where(ae<delta,e*e/2,delta*(ae-delta/2)))*ys**2))
            g = np.clip(e,-delta,delta)/len(ix)
            gb = (g@p[4].T)*(1-bb**2)
            ga = (gb@p[2].T)*(1-aa**2)
            grads = [xx.T@ga,ga.sum(0),aa.T@gb,gb.sum(0),bb.T@g,g.sum(0)]
            step += 1
            for i,d in enumerate(grads):
                d = d + (CONFIG["l2"]*p[i] if i%2==0 else 0)
                m[i] = .9*m[i]+.1*d
                vv[i] = .999*vv[i]+.001*d*d
                p[i] -= CONFIG["learning_rate"]*(m[i]/(1-.9**step))/(np.sqrt(vv[i]/(1-.999**step))+1e-8)
        trace.append(dict(epoch=epoch,dev_huber_raw2=np.mean(losses)))
    return p,center,scale,y0,ys,trace


def predict(model,x):
    params,mean,scale,y0,ys,_ = model
    return forward((x-mean)/scale,params)[2][:,0]*ys+y0


def per_unit(err,groups):
    a = pd.DataFrame(dict(unit=groups,error=err,abs_error=np.abs(err),sq=err**2))
    return a.groupby("unit",as_index=False,sort=False).agg(
        samples=("error","size"),bias=("error","mean"),mae=("abs_error","mean"),mse=("sq","mean"),
        p95=("abs_error",lambda x:float(np.quantile(x,.95))))


def evaluate(rows, err):
    unit = per_unit(err,rows.unit.to_numpy())
    n = len(unit)
    if not n:
        return dict(rows=0,units=0,rmse=np.nan,mae=np.nan,p95=np.nan,bias=np.nan)
    counts = unit.set_index("unit").samples
    w = 1/rows.unit.map(counts).to_numpy()
    sort = np.argsort(np.abs(err))
    index = np.searchsorted(np.cumsum(w[sort]),.95*w.sum())
    return dict(rows=len(rows),units=n,rmse=float(np.sqrt(unit.mse.mean())),mae=float(unit.mae.mean()),
                p95=float(np.abs(err)[sort[index]]),bias=float(unit.bias.mean()))


def scores(table, prediction, model, seed):
    all_rows = []
    for cohort in ["ALL","B1","C","D"]:
        scope = table if cohort=="ALL" else table[table.cohort.eq(cohort)]
        for phase in ["ALL",*PHASES]:
            rows = scope if phase=="ALL" else scope[scope.phase.eq(phase)]
            error = rows.current_raw.to_numpy()-prediction[rows.index]
            all_rows.append(dict(model=model,seed=seed,cohort=cohort,phase=phase,**evaluate(rows,error)))
    return all_rows


def main():
    table = pd.read_csv(OUT/"joint_samples.csv.gz",low_memory=False)
    valid = table.loc[table.candidate].copy()
    dev = valid.loc[valid.split.eq("dev")].reset_index(drop=True)
    test = valid.loc[valid.split.eq("internal")].reset_index(drop=True)
    assert not set(dev.unit)&set(test.unit)
    w = weights(dev)
    y = dev.label_raw.to_numpy()
    config = dict(CONFIG,dataset_sha256=sha(OUT/"joint_samples.csv.gz"),features=MODELS,
                  normalization="weighted training-only statistics", dev_rows=len(dev), dev_units=dev.unit.nunique(),
                  internal_rows=len(test), internal_units=test.unit.nunique(),
                  hold_protocol="zero inertia and sliding friction on static HOLD",model_version="internal research v1",
                  numpy=np.__version__,pandas=pd.__version__)
    (OUT/"training_config.json").write_text(json.dumps(config,indent=2),encoding="utf-8")
    pred = {"M0":test.m0_raw.to_numpy(),"M1":test.m1_raw.to_numpy()}
    result = [r for name,p in pred.items() for r in scores(test,p,name,SEEDS[0])]
    losses = []
    model_dir = OUT/"models"
    model_dir.mkdir(exist_ok=True)
    for name,features in MODELS.items():
        x,xt = dev[features].to_numpy(),test[features].to_numpy()
        for seed in SEEDS:
            model = train(x,y,w,seed)
            p,mean,scale,y0,ys,trace = model
            estimate = test.m1_raw.to_numpy()+predict(model,xt)
            result.extend(scores(test,estimate,name,seed))
            losses.extend(dict(r,model=name,seed=seed) for r in trace)
            if seed==SEEDS[0]:
                path = model_dir/(name+".npz")
                np.savez(path,features=np.array(features),x_mean=mean,x_scale=scale,
                         y_mean=y0,y_scale=ys,**dict(zip(("W1","b1","W2","b2","W3","b3"),p)))
                with np.load(path,allow_pickle=False) as saved:
                    check = forward((xt-saved["x_mean"])/saved["x_scale"],
                                    [saved[k] for k in ("W1","b1","W2","b2","W3","b3")])[2][:,0]
                    assert np.allclose(check*saved["y_scale"]+saved["y_mean"],estimate-test.m1_raw)
                pred[name] = estimate
            print(name,seed,round(result[-28]["rmse"],2),flush=True)
    metrics = pd.DataFrame(result)
    csv("evaluation.csv",metrics.loc[metrics.seed.eq(SEEDS[0])])
    csv("seed_sensitivity.csv",metrics)
    csv("training_loss.csv",pd.DataFrame(losses))
    detail = []
    for name,yp in pred.items():
        for (cohort,phase),sub in test.groupby(["cohort","phase"]):
            u = per_unit(sub.current_raw.to_numpy()-yp[sub.index],sub.unit.to_numpy())
            u["model"],u["cohort"],u["phase"] = name,cohort,phase
            detail.append(u)
    csv("evaluation_by_trial.csv",pd.concat(detail,ignore_index=True))
    diagnostic = []
    for (cohort,phase),sub in table.loc[table.split.eq("internal") & ~table.candidate &
            table.phase.isin(["startup","near_zero"]) & table.m0_raw.notna()].groupby(["cohort","phase"]):
        diagnostic.append(dict(cohort=cohort,phase=phase,model="M0_only_unmatched",
                               **evaluate(sub,(sub.current_raw-sub.m0_raw).to_numpy())))
    csv("unsupported_diagnostics.csv",pd.DataFrame(diagnostic))
    prediction = test[["cohort","unit","trace","session","round","phase","segment","frame","card_tick_raw","current_raw"]].copy()
    for name,yp in pred.items():
        prediction[name] = yp
    prediction.to_csv(OUT/"internal_predictions.csv.gz",index=False,float_format="%.10g",
                      compression=dict(method="gzip",compresslevel=3,mtime=0))
    print(metrics.loc[metrics.seed.eq(SEEDS[0])&metrics.phase.eq("ALL")].to_string(index=False))


if __name__=="__main__":
    main()
