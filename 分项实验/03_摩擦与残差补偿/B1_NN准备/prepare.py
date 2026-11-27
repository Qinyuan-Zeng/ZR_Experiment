"""Offline feature/target export; no model fitting. Run with python -X utf8 -B."""
from pathlib import Path
from functools import lru_cache
import hashlib
import json
import sys
import numpy as np
import pandas as pd

OUT = Path(__file__).resolve().parent
ROOT = OUT.parent
sys.dont_write_bytecode = True
sys.path.insert(0,str(ROOT/"B0"))
from b0_sources import POSITION, ALPHA, FRICTION
from b0_states import friction_and_residual

def read(p):
    return json.loads(p.read_text(encoding="utf-8-sig"))

def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()

def save(name, frame):
    frame.to_csv(OUT/name,index=False,encoding="utf-8-sig")

features=["x_pulse","v_pulse_s","a_pulse_s2","v_history","still_history","arrival_memory"]
dt=.005

def derivative(x,n):
    # Endpoint quadratic: offsets -n+1 ... 0 on a 5 ms encoder grid.
    offsets=np.arange(1-n,1,dtype=float)
    weights=np.linalg.pinv(np.column_stack([np.ones(n),offsets,offsets**2]))
    values=np.lib.stride_tricks.sliding_window_view(x,n)
    v=np.r_[np.full(n-1,np.nan),values@weights[1]/dt]
    a=np.r_[np.full(n-1,np.nan),values@weights[2]*2/dt**2]
    return v,a

def histories(v):
    z=np.zeros((len(v),3))
    state=np.array([0.,1.,0.])
    for j,speed in enumerate(v):
        if np.isfinite(speed):
            update=-np.expm1(-np.array([dt/.1,dt/.1,abs(speed)*dt/250.]))
            state+=update*(np.array([speed,np.exp(-(speed/200)**2),np.tanh(speed/500)])-state)
        z[j]=state
    return z.T

@lru_cache(None)
def attempt_data(rev,attempt):
    p=ROOT/"B1/manifest"/rev/"attempts"/attempt/"acceptance.json"
    return {r["slot_id"]:r for r in read(p)["accepted"]}

prov=read(ROOT/"B0/audit_provenance.json")
for p,k in [(POSITION,"position_model_sha256"),(ALPHA,"alpha_source_sha256"),(FRICTION,"friction_model_sha256")]:
    assert sha(p)==prov[k]
ip=read(POSITION); alpha=read(ALPHA)["alpha"]; f1=read(FRICTION)["models"]["F1"]
ledger=ROOT/"B1_audit/legs.csv"
legs=pd.read_csv(ledger).query("selected and status=='valid'")
assert len(legs)==320 and legs.slot.nunique()==160
rows=[]; sources=[]
for leg in legs.itertuples():
    accepted=attempt_data(leg.rev,leg.attempt)[leg.slot]
    hashes=accepted["leg_inputs"]["positive" if leg.dir==1 else "negative"]
    base=ROOT/"B1/data/raw"/leg.rev/leg.attempt
    prefix=f"{leg.slot}_leg{leg.leg}"
    raw=base/(prefix+".csv"); meta=base/(prefix+".txt")
    assert sha(raw)==hashes["watch"]["sha256"] and sha(meta)==hashes["metadata"]["sha256"]
    m=dict(line.split("=",1) for line in meta.read_text(encoding="utf-8-sig").splitlines() if "=" in line)
    w=pd.read_csv(raw); ticks=w.card_tick_raw.to_numpy(dtype=np.int64)
    assert len(w)==leg.native_frames and np.all(np.diff(ticks)%2**32==1)
    k=np.arange(0,len(w),100)
    def when(key): return (int(m[key+"_tick"])-int(ticks[0]))%2**32
    cmd=when("command_after"); settled=when("settled")
    x=w.encoder_position_raw.to_numpy()[k]; current=w.current_feedback_raw.to_numpy()[k]
    v,a=derivative(x,5); alt_v,alt_a=derivative(x,7)
    hv,hs,hm=histories(v)
    pre=k<when("command_before"); post=k>=settled
    hold=(pre|post)&(abs(v)<=50)
    phase=np.full(len(k),"transition",dtype="U16")
    motion=(k>=cmd)&(k<settled)
    phase[motion&(abs(v)>=2000)&(abs(a)<30000)]="cruise"
    phase[motion&(leg.dir*a>=30000)]="accelerate"
    phase[motion&(leg.dir*a<=-30000)]="brake"
    phase[motion&(k-cmd<400)]="startup"
    phase[pre]="pre_hold";phase[post]="post_hold"
    mech=friction_and_residual(x,current,v,a,hold,ip,alpha,f1,prov["F1_position_pulse"])
    alt=friction_and_residual(x,current,alt_v,alt_a,(pre|post)&(abs(alt_v)<=50),ip,alpha,f1,prov["F1_position_pulse"])
    short=(leg.v/leg.a<=.020)&motion&(phase!="cruise")
    stable=np.isfinite(alt_a)&(abs(a-alt_a)<=np.maximum(30000,.20*np.maximum(abs(a),abs(alt_a))))
    in_alpha=(abs(a)<30000)|((abs(a)>=400000)&(abs(a)<=600000))
    valid=mech["mechanism_domain_valid"]
    history_known=k>=cmd
    strict=(valid & in_alpha & stable & ~short & history_known & ~np.isin(phase,["transition"])
            & (~post|hold) & np.isfinite(alt["r_raw"]) & (abs(mech["I_f_raw"]-alt["I_f_raw"])<5))
    session=accepted["field_session"]
    d=pd.DataFrame(dict(slot=leg.slot,leg=leg.leg,session=session,split="dev" if leg.period=="S1" else "internal",
        path=leg.path,speed=leg.v,acc=leg.a,round=leg.round,direction=leg.dir,frame=k,tick=ticks[k],phase=phase,
        x_pulse=x,v_pulse_s=v,a_pulse_s2=a,v_history=hv,still_history=hs,arrival_memory=hm,
        current_raw=current,ip_raw=mech["I_p_raw"],ia_raw=mech["I_a_raw"],if_raw=mech["I_f_raw"],
        m0_raw=mech["I_M0_raw"],m0_residual_raw=mech["r_M0_raw"],mech_raw=mech["I_mech_raw"],residual_raw=mech["r_raw"],
        alt_residual_raw=alt["r_raw"],mechanism_valid=valid,alpha_supported=in_alpha,acc_consistent=stable,
        short_ramp=short,ip_inside=mech["ip_position_valid"],f1_x_inside=mech["f1_position_valid"],
        near_zero=mech["f1_near_zero_unresolved"],above_speed=mech["f1_above_40"],hold_zero=hold,
        history_observed=history_known,candidate=strict))
    rows.append(d)
    sources.append([leg.slot,leg.leg,session,str(raw.relative_to(ROOT)),hashes["watch"]["sha256"],hashes["metadata"]["sha256"]])

t=pd.concat(rows,ignore_index=True)
assert t.groupby("slot").split.nunique().max()==1 and t.groupby("session").split.nunique().max()==1
assert t.groupby("split").session.nunique().eq(1).all()
assert np.allclose(t.loc[t.candidate,"current_raw"]-t.loc[t.candidate,"mech_raw"],t.loc[t.candidate,"residual_raw"])
assert t.loc[t.candidate,features].notna().all().all()
t.to_csv(OUT/"samples.csv.gz",compression=dict(method="gzip",compresslevel=3,mtime=0),index=False,float_format="%.10g")
save("sources.csv",pd.DataFrame(sources,columns=["slot","leg","session","raw_file","watch_sha256","metadata_sha256"]))
save("splits.csv",t[["slot","session","split","path","speed","acc","round"]].drop_duplicates())
save("coverage.csv",t.groupby(["split","phase","path","speed","acc","direction"]).agg(
    samples=("slot","size"),legs=("slot","nunique"),mechanism=("mechanism_valid","sum"),candidate=("candidate","sum"),
    consistent=("acc_consistent","sum"),alpha_supported=("alpha_supported","sum"),short=("short_ramp","sum"),
    near_zero=("near_zero","sum"),above=("above_speed","sum")).reset_index())
save("phase_counts.csv",t.groupby("phase").agg(samples=("slot","size"),mechanism=("mechanism_valid","sum"),candidate=("candidate","sum")).reset_index())
stats=[]
for support,mask in [("mechanism",t.mechanism_valid),("candidate",t.candidate)]:
    for keys,g in t[mask].groupby(["slot","leg","session","split","path","speed","acc","direction","phase"]):
        r=g.residual_raw.to_numpy()
        stats.append([support,*keys,len(r),r.mean(),np.mean(abs(r)),np.mean(r*r),np.quantile(abs(r),.95)])
u=pd.DataFrame(stats,columns=["support","slot","leg","session","split","path","speed","acc","direction","phase","n","bias","mae","mse","p95"])
save("leg_phase.csv",u)
keys=["support","phase","direction","path","speed","acc"]
g=u.groupby(keys+["split"]).agg(nlegs=("slot","size"),bias=("bias","mean"),sd=("bias","std")).unstack("split")
g.columns=["_".join(k) for k in g.columns]
g["shift"]=g.bias_internal-g.bias_dev
save("repeatability.csv",g.reset_index())
summary=u.groupby(["support","phase","direction"]).agg(legs=("slot","size"),bias=("bias","mean"),mae=("mae","mean"),mse=("mse","mean"),median_leg_p95=("p95","median")).reset_index()
summary["rmse"]=np.sqrt(summary.mse)
summary=summary.merge(g.reset_index().groupby(["support","phase","direction"])[["sd_dev","shift"]].median().reset_index())
save("summary.csv",summary)
# Fixed-state grouping for reproducibility, without fitting any residual predictor.
t["bin"]=np.floor(t.x_pulse/500)
cr=t[t.candidate & t.phase.eq("cruise")].groupby(["split","slot","path","speed","acc","direction","bin"]).residual_raw.mean()
b=cr.groupby(["split","path","speed","acc","direction","bin"]).mean().unstack("split").dropna()
shape=[]
for key,h in b.groupby(["path","speed","acc","direction"]):
    if len(h)>=4:
        shape.append([*key,len(h),h.dev.corr(h.internal),np.sqrt(np.mean((h.dev-h.internal)**2))])
save("cruise_shape.csv",pd.DataFrame(shape,columns=["path","speed","acc","direction","bins","correlation","difference_rms"]))
(OUT/"features.json").write_text(json.dumps(dict(inputs=features,target="residual_raw",selection="candidate",splits={"dev":"S1","internal":"S2"},dt_s=dt,
    hashes={p.name:sha(p) for p in [POSITION,ALPHA,FRICTION,ledger,Path(__file__)]}),indent=2),encoding="utf8")
