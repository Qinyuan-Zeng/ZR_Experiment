"""Rebuild a 5 ms joint dataset from accepted native Watch records."""
from collections import Counter
from pathlib import Path
import hashlib
import json

import numpy as np
import pandas as pd

OUT = Path(__file__).resolve().parent
ROOT = OUT.parent
DT = .005
TICK = .00005
FEATURES = ["x_pulse", "v_pulse_s", "a_pulse_s2", "v_history", "still_history", "arrival_memory", "time_since_stop"]
PROTOCOLS = {"B1":"Z-B1-20261127-02", "C":"Z-C-HISTORY-01", "D":"Z-D-HOLD-01"}
POSITION = ROOT.parent / "01_位置相关空载电流辨识/models/position_B_primary_v1.json"
ALPHA = ROOT.parent / "02_等效惯性电流辨识/results/d/Z-DP_20261126-095126_343_29884_summary.json"
FRICTION = ROOT / "manifest/formal_a/Z-AF-20261126-03/models_frozen.json"


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def sha(path):
    with path.open("rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()


def csv(name, frame):
    frame.to_csv(OUT/name, index=False, encoding="utf-8-sig")


def checked_protocol(path):
    p = path/"protocol.json"
    assert sha(p) == (path/"protocol.sha256").read_text().strip(), p
    result = read(p)
    assert result["core_period_ns"] == 50000
    return result


def sources():
    old = checked_protocol(ROOT/"B1/manifest/Z-B1-20261127-01")
    for cohort, protocol in PROTOCOLS.items():
        folder = ROOT/cohort/"manifest"/protocol
        frozen = checked_protocol(folder)
        b1 = cohort == "B1"
        if b1:
            assert old["slots"] == frozen["slots"] and old["limits"] == frozen["limits"]
        id_col = "slot_id" if b1 else "id"
        slots = {s["id"]:s for s in frozen["slots" if b1 else "trials"]}
        table = pd.read_csv(ROOT/cohort/"results"/protocol/("accepted_pairs.csv" if b1 else "accepted_trials.csv"))
        accepted = table.set_index(id_col, verify_integrity=True).to_dict("index")
        seen = set()
        for file in sorted((folder/"attempts").glob("*/acceptance.json")):
            evidence = read(file)
            if not evidence["accepted"]:
                continue
            ledger = evidence.get("batch") or {name: evidence[name.split(".")[0]+"_sha256"]
                                               for name in ("pre.txt","post.txt","native.txt","request.json")}
            for name, digest in ledger.items():
                assert sha(file.parent/name) == digest, file.parent/name
            inherited = evidence.get("inherited_from")
            if inherited:
                parent = ROOT/"B1/manifest"/inherited["protocol"]/"attempts"/inherited["attempt"]/"acceptance.json"
                assert sha(parent) == inherited["acceptance_sha256"], parent
            for r in evidence["accepted"]:
                key = r[id_col]
                assert key in slots and key not in seen and key in accepted
                table_row = accepted[key]
                assert r["field_session"] == table_row["field_session"]
                attempt_col = "attempt_id" if b1 else "attempt"
                assert r[attempt_col] == table_row[attempt_col]
                seen.add(key)
                if b1:
                    assert r["round"] == slots[key]["round"] and r["period"] == slots[key]["period"]
                    group = [({Path(v["path"]):v["sha256"] for v in r["leg_inputs"][side].values()},
                              sign, r[side+"_native_frames"]) for side,sign in (("positive",1),("negative",-1))]
                    round_id = r["round"]
                    speed = r["speed_mm_s"]
                    acc = r["acc_mm_s2"]
                else:
                    assert all(r[k] == v for k,v in slots[key].items())
                    group = [({Path(k):v for k,v in r["inputs"].items()},r["sign"],r["frames"])]
                    round_id = r["repeat"]
                    speed = r["speed"] if cohort=="C" else 10
                    acc = 400
                for inputs, sign, frames in group:
                    for path, digest in inputs.items():
                        assert sha(path) == digest, path
                    raw = next(p for p in inputs if p.suffix==".csv" and not p.name.endswith((".sdk.csv",".batches.csv")))
                    meta = next(p for p in inputs if p.suffix==".txt")
                    record = dict(cohort=cohort, unit=f"{cohort}:{key}", trace=f"{cohort}:{raw.stem}",
                                  round=round_id, session=r["field_session"],
                                  split="dev" if round_id <= (3 if b1 else 2) else "internal",
                                  speed=speed, acceleration=acc, direction=sign, path=r.get("path",""),
                                  history=r.get("history",""), dwell_ms=r.get("dwell",0),
                                  actual_dwell_ms=r.get("arrival_to_restart_ms",np.nan),
                                  raw=raw, meta=meta, frames=frames, acceptance=file)
                    yield record, inputs
        assert seen == set(slots) == set(accepted) and len(seen)=={"B1":160,"C":126,"D":18}[cohort]


def derivative(x, n):
    t = np.arange(1-n,1,dtype=float)
    coef = np.linalg.pinv(np.column_stack([np.ones(n),t,t*t]))
    values = np.lib.stride_tricks.sliding_window_view(x,n)
    return (np.r_[np.full(n-1,np.nan),values@coef[1]/DT],
            np.r_[np.full(n-1,np.nan),values@coef[2]*2/DT**2])


def history_features(x,v):
    z = np.zeros((len(v),4))
    state = np.array([0.,1.,0.])
    clock = 0.
    anchor = None
    for j,speed in enumerate(v):
        if np.isfinite(speed):
            gain = -np.expm1(-np.array([DT/.1, DT/.1, abs(speed)*DT/250]))
            state += gain*(np.array([speed,np.exp(-(speed/200)**2),np.tanh(speed/500)])-state)
            if anchor is not None and (abs(speed)>400 or abs(x[j]-anchor)>10):
                anchor = None
                clock = 0.
            if anchor is not None:
                clock += DT
            elif j>=6 and abs(speed)<=200 and np.ptp(x[j-6:j+1])<=6:
                anchor = x[j]
        z[j] = (*state,clock)
    return z.T


def build(rec, ip, alpha, f1, grid):
    w = pd.read_csv(rec["raw"])
    ticks = w.card_tick_raw.to_numpy(dtype=np.int64)
    elapsed = (ticks-ticks[0])%2**32
    assert len(w)==rec["frames"] and np.array_equal(w.frame,np.arange(len(w))) and np.array_equal(elapsed,w.frame)
    meta = dict(line.split("=",1) for line in rec["meta"].read_text(encoding="utf-8-sig").splitlines() if "=" in line)
    assert meta["outcome"]=="complete" and int(meta["core_period_ns"])==50000 and int(meta["direction"])==rec["direction"]
    def event(name):
        index = (int(meta[name+"_tick"])-int(ticks[0]))%2**32
        assert 0<=index<len(ticks), (rec["trace"],name,index)
        return index
    k = np.arange(0,len(w),100)
    x = w.encoder_position_raw.to_numpy()[k]
    current = w.current_feedback_raw.to_numpy()[k]
    assert np.isfinite(x).all() and np.isfinite(current).all()
    v,a = derivative(x,5)
    v_alt,a_alt = derivative(x,7)
    hv,hs,hm,ts = history_features(x,v)
    phase = np.full(len(k),"transition",dtype="U12")
    segment = np.full(len(k),"pre",dtype="U12")
    known = np.zeros(len(k),bool)
    short = known.copy()
    moving = known.copy()
    hold_region = known.copy()
    hold_age = np.full(len(k),np.nan)
    cohort = rec["cohort"]
    sign = rec["direction"]
    if cohort=="B1":
        motion = [("move","command_after","settled",sign,rec["speed"],rec["acceleration"])]
        holds = [("post","settled","post_hold_complete",int(meta["end_pulse"]))]
    elif cohort=="C":
        direct = rec["history"]=="direct"
        approach_sign = -sign if rec["history"]=="reverse" else sign
        motion = [("approach","approach_after","approach_settled",approach_sign,2 if direct else 10,20 if direct else 400),
                  ("probe","probe_after","probe_settled",sign,rec["speed"],400)]
        holds = [("dwell","approach_settled","dwell_end",16000),("post","probe_settled","post_hold_end",int(meta["probe_target_readback_pulse"]))]
    else:
        motion = [("approach","approach_after","approach_settled",sign,10,400)]
        holds = [("post","approach_settled","hold_end",int(meta["target_pulse"]))]
    known = k>=event(motion[0][1])
    for name,start,end,direction,speed,acc in motion:
        begin,finish = event(start),event(end)
        assert begin<finish
        use = (k>=begin)&(k<finish)
        moving |= use
        segment[use] = name
        stage = np.select([direction*a>=30000,direction*a<=-30000],["accelerate","brake"],default="cruise")
        stage = np.where(np.abs(v)<2000,"near_zero",stage)
        phase[use] = stage[use]
        phase[use & (k-begin<400)] = "startup"
        short |= use & (speed/acc<=.020) & (stage!="cruise")
    phase[~known] = "pre_hold"
    for role,start,end,target in holds:
        begin,finish = event(start),event(end)
        use = (k>=begin)&(k<=finish)
        assert begin<finish and not (use & moving).any()
        segment[use] = role
        phase[use] = "hold"
        hold_age[use] = (k[use]-begin)*TICK
        hold_region |= use & (np.abs(x-target)<=10)
    static = hold_region & (np.abs(v)<=200) & (np.abs(v_alt)<=200)
    lo,hi = ip["range_pulse"]
    ip_valid = (x>=lo)&(x<=hi)
    ip_raw = np.where(ip_valid,np.interp(x,ip["node_pulse"],ip["current_raw"]),np.nan)
    inertia = np.where(static,0,alpha*a)
    m0 = ip_raw+inertia
    speed = np.abs(v)
    in_fx = (x>=grid[0])&(x<=grid[1])
    f = np.full(len(k),np.nan)
    f[static] = 0
    f_alt = np.full(len(k),np.nan)
    f_alt[static] = 0
    for direction in (-1,1):
        intercept,slope = f1[str(direction)]
        for vel,dest in ((v,f),(v_alt,f_alt)):
            s = direction*vel
            mask = moving & in_fx & (s>=2000)&(s<=40000)
            dest[mask] = intercept+slope*s[mask]/1000
    m1 = m0+f
    stable = np.abs(a-a_alt)<=np.maximum(30000,.2*np.maximum(np.abs(a),np.abs(a_alt)))
    alpha_ok = (np.abs(a)<30000)|((np.abs(a)>=390000)&(np.abs(a)<=610000))
    dynamic_ok = (moving & (cohort!="D") & stable & alpha_ok & ~short &
                  (np.abs(f-f_alt)<5) & np.isfinite(v)&np.isfinite(a))
    candidate = known & np.isfinite(m1) & (static|dynamic_ok)
    near = moving & (speed<2000)
    assert not (candidate & (near | short)).any()
    columns = {key:rec[key] for key in ("cohort","unit","trace","round","session","split","speed","acceleration",
                                       "direction","path","history","dwell_ms","actual_dwell_ms")}
    return pd.DataFrame(dict(columns, frame=k, card_tick_raw=ticks[k], time_s=k*TICK, phase=phase, segment=segment,
        hold_age_s=hold_age,x_pulse=x,v_pulse_s=v,a_pulse_s2=a,v_history=hv,still_history=hs,
        arrival_memory=hm,time_since_stop=ts, current_raw=current,ip_raw=ip_raw,ia_raw=inertia,if_raw=f,
        m0_raw=m0,m1_raw=m1,mechanism_error_raw=current-m1,label_raw=np.where(candidate,current-m1,np.nan),
        candidate=candidate,near_zero=near,short_ramp=short,history_known=known,hold_zero=static,
        acceleration_supported=alpha_ok,dual_window_agrees=stable,ip_valid=ip_valid,f1_position_valid=in_fx,
        f1_sliding_speed_valid=moving&(speed>=2000)&(speed<=40000),mechanism_valid=np.isfinite(m1)))


def main():
    prov = read(ROOT/"B0/audit_provenance.json")
    for path,key in ((POSITION,"position_model_sha256"),(ALPHA,"alpha_source_sha256"),(FRICTION,"friction_model_sha256")):
        assert sha(path)==prov[key],path
    ip=read(POSITION)
    alpha=read(ALPHA)["alpha"]
    f1=read(FRICTION)["models"]["F1"]
    data, manifest = [], []
    for rec, inputs in sources():
        data.append(build(rec,ip,alpha,f1,prov["F1_position_pulse"]))
        manifest.append(dict({k:v for k,v in rec.items() if k not in ("raw","meta","acceptance")},
            raw=str(rec["raw"].relative_to(ROOT)),meta=str(rec["meta"].relative_to(ROOT)),
            acceptance=str(rec["acceptance"].relative_to(ROOT)),
            acceptance_sha256=sha(rec["acceptance"]),inputs=json.dumps({str(p.relative_to(ROOT)):h for p,h in inputs.items()})))
    assert Counter(s["cohort"] for s in manifest)=={"B1":320,"C":126,"D":18}
    assert len(set(s["raw"] for s in manifest))==len(manifest)
    t=pd.concat(data,ignore_index=True)
    assert t.groupby("unit").split.nunique().max()==1 and np.isfinite(t.loc[t.candidate,FEATURES]).all().all()
    assert t.label_raw.notna().equals(t.candidate)
    # Prefix equality tests the feature implementation's no-future contract.
    sample=data[321].x_pulse.to_numpy()
    n=len(sample)//2
    assert np.allclose(np.array(history_features(sample[:n],derivative(sample[:n],5)[0])),
                       np.array(history_features(sample,derivative(sample,5)[0]))[:,:n])
    t.to_csv(OUT/"joint_samples.csv.gz",index=False,float_format="%.10g",
             compression=dict(method="gzip",compresslevel=3,mtime=0))
    csv("sources.csv",pd.DataFrame(manifest))
    csv("splits.csv",t[["unit","cohort","session","round","split"]].drop_duplicates())
    csv("coverage.csv",t.groupby(["cohort","split","phase"],as_index=False).agg(
        rows=("unit","size"),units=("unit","nunique"),eligible=("candidate","sum"),
        near_zero=("near_zero","sum"),short_ramp=("short_ramp","sum"),
        mechanism_valid=("mechanism_valid","sum")))
    (OUT/"data_config.json").write_text(json.dumps(dict(
        features=FEATURES,models={p.name:sha(p) for p in (POSITION,ALPHA,FRICTION)}, alpha=alpha,
        protocols={cohort:sha(ROOT/cohort/"manifest"/name/"protocol.json") for cohort,name in PROTOCOLS.items()},
        dt_s=DT,native_tick_s=TICK, accepted_frames=sum(m["frames"] for m in manifest),rows=len(t),
        E_used=False),indent=2),encoding="utf-8")
    print(len(t),"rows",t.groupby(["cohort","split"]).candidate.agg(["size","sum"]).to_string())


if __name__=="__main__":
    main()
