"""Offline audit. Run with Python -B. All exports stay beside this script."""
import hashlib
import json
import sys
from pathlib import Path
import numpy as np
import pandas as pd

OUT = Path(__file__).resolve().parent
ROOT = OUT.parent
sys.dont_write_bytecode = True
sys.path.insert(0,str(ROOT/"scripts"))
from b1_replay import check_leg, fields

B = ROOT/"B1"
FINAL = "Z-B1-20261127-02"
proto = json.loads((B/"manifest"/FINAL/"protocol.json").read_text())
slots = {s["id"]:s for s in proto["slots"]}
ledger = pd.read_csv(B/"results"/FINAL/"accepted_pairs.csv")
accepted = dict(zip(ledger.slot_id, ledger.attempt_id))
assert len(accepted)==len(ledger)==len(slots) and set(accepted)==set(slots)
legs, attempts, guards = [], [], []
for folder in sorted((B/"manifest").glob("*/attempts/*")):
    if not folder.is_dir(): continue
    rev, aid = folder.parents[1].name,folder.name
    raw=B/"data/raw"/rev/aid
    req=json.loads((folder/"request.json").read_text(encoding="utf-8-sig"))
    accfile=folder/"acceptance.json"
    record=json.loads(accfile.read_text(encoding="utf-8-sig")) if accfile.exists() else {}
    historical={r["slot_id"]:r for r in record.get("accepted",[])}
    resolution=folder/"recovery_resolution.json"
    recovery=json.loads(resolution.read_text()).get("reason","") if resolution.exists() else ""
    attempts.append(dict(revision=rev,attempt=aid,requested=len(req["slots"]),locally_accepted=len(historical),
                         recovery=recovery,rejected=str(record.get("rejected",{})),exit=(folder/"exit.json").read_text().strip() if (folder/"exit.json").exists() else ""))
    if (raw/"guards.csv").exists():
        g=pd.read_csv(raw/"guards.csv")
        guards.append(dict(attempt=aid,n=len(g),temp_read=len(g.card_temp_c.dropna()),temp_min=g.card_temp_c.min(),temp_max=g.card_temp_c.max(),
                           feedback_abs_max=g.current_raw.abs().max(),estimated_speed_max=g.estimated_abs_speed_mm_s.max()))
    for slot in req["slots"]:
        key=slot["id"]
        assert slot==slots[key]
        for leg in [1,2]:
            stem=key+f"_leg{leg}"
            watch=raw/(stem+".csv")
            row=dict(slot=key,rev=rev,attempt=aid,leg=leg,period=slot["period"],path=slot["path"],v=slot["v"],a=slot["a"],round=slot["round"],
                     dir=slot["first"]*(1 if leg==1 else -1),selected=accepted[key]==aid,watch_present=watch.exists(),recovery=recovery)
            legs.append(row)
            if not watch.exists():
                row.update(status="unstarted",reason="no Watch file")
                continue
            meta=fields(raw/(stem+".txt"))
            row["native_reason"]=meta.get("reason")
            row["outcome"]=meta.get("outcome")
            try:
                stats,hashes=check_leg(raw,slot,leg,proto["limits"])
                row.update(stats)
                if row["selected"]:
                    frozen=historical[key]["leg_inputs"]["positive" if row["dir"]==1 else "negative"]
                    assert all(hashes[k]["sha256"]==frozen[k]["sha256"] for k in hashes)
                row["status"]="valid" if row["selected"] else "complete_unpaired"
            except (ValueError,KeyError,AssertionError,RuntimeError) as e:
                row.update(status="invalid",reason=str(e))
                continue
            w=pd.read_csv(watch)
            x=w.encoder_position_raw.to_numpy(); i=w.current_feedback_raw.to_numpy()
            tick=int(w.card_tick_raw.iloc[0])
            def index(k): return (int(meta[k+"_tick"])-tick)%2**32
            start,stop=index("command_after"),index("settled")
            idx=np.arange(200,len(x)-200,100)
            z=x[idx[:,None]+np.arange(-2,3)*100]
            v=z@np.array([-2,-1,0,1,2])/50
            a=z@np.array([2,-1,-2,-1,2])/(7*.005**2*1000)
            motion=(idx>=start)&(idx<=stop)&(x[idx]>=slot["lo"])&(x[idx]<=slot["hi"])
            near=abs(row["dir"]*v-slot["v"])<=max(.25,.05*slot["v"])
            joint=motion & near & (abs(abs(a)-slot["a"])<=.2*slot["a"])
            dynamic=motion & (row["dir"]*v>0)&(abs(abs(a)-slot["a"])<=.2*slot["a"])
            for phase,direction in [("accelerating",1),("braking",-1)]:
                pick=dynamic & (direction*a*row["dir"]>0) & (abs(v)>=.25*slot["v"]) & (abs(v)<=.75*slot["v"])
                row[phase+"_raw"]=np.mean(i[idx[pick]]) if pick.any() else np.nan
            cruise=motion & near & (abs(a)<.2*slot["a"]) & (abs(x[idx]-(slot["lo"]+slot["hi"])/2)<(slot["hi"]-slot["lo"])*.2)
            host=pd.read_csv(raw/(stem+".sdk.csv"))
            times=["command_before","command_after","settled","post_hold_complete"]
            clock=[(int(meta[k+"_host_us"])-int(meta["watch_before_host_us"]))/1000-
                   ((int(meta[k+"_tick"])-int(meta["watch_before_tick"]))%2**32)*.05 for k in times]
            row.update(joint=joint.sum(),acc_points=(dynamic&(a*row["dir"]>0)).sum(),dec_points=(dynamic&(a*row["dir"]<0)).sum(),
                       cruise_raw=i[idx[cruise]].mean() if cruise.any() else np.nan,
                       pre_raw=i[start-10000:start-100].mean(),post_raw=i[stop+100:stop+10000].mean(),
                       peak_step_raw=np.abs(np.diff(i)).max(),min_raw=i.min(),max_raw=i.max(),clock_discrepancy_ms=max(map(abs,clock)),
                       host_gap_max_ms=host[host.phase=="motion"].host_begin_us.diff().max()/1000,
                       trap_acc=float(meta["measurement_trap_acc_pulse_ms2"])*1000,
                       trap_dec=float(meta["measurement_trap_dec_pulse_ms2"])*1000,
                       readback_ok=float(meta["measurement_speed_readback_pulse_ms"])==slot["v"] and int(meta["measurement_target_readback_pulse"])==int(meta["end_pulse"]),
                       following_max=host.following_error_pulse.abs().max())

def write(df,name): df.to_csv(OUT/(name+".csv"),index=False,encoding="utf-8-sig")
t=pd.DataFrame(legs); write(t,"legs"); write(pd.DataFrame(attempts),"attempts");write(pd.DataFrame(guards),"guards")
s=t[t.selected & t.status.eq("valid")].copy()
groups=[]
for (p,v,a),g in s.groupby(["path","v","a"]):
    periods=g.groupby(["period","dir"]).cruise_raw.mean().unstack(0)
    reps=g.groupby("dir").cruise_raw.std()
    groups.append(dict(path=p,v=v,a=a,legs=len(g),pairs=sum(len(b)==2 and set(b.dir)=={-1,1} for _,b in g.groupby("slot")),S1=g[g.period=="S1"].slot.nunique(),S2=g[g.period=="S2"].slot.nunique(),
        pos_joint=int(g[g.dir==1].joint.sum()),neg_joint=int(g[g.dir==-1].joint.sum()),min_acc_points=g.acc_points.min(),min_dec_points=g.dec_points.min(),
        pos_sd_raw=reps[1],neg_sd_raw=reps[-1],pos_shift_raw=periods.loc[1,"S2"]-periods.loc[1,"S1"],neg_shift_raw=periods.loc[-1,"S2"]-periods.loc[-1,"S1"]))
c=pd.DataFrame(groups);write(c,"conditions")
phase_rows=[]
for keys,block in s.groupby(["path","v","a","dir"]):
    for phase in ["accelerating","cruise","braking","pre","post"]:
        selected=block.dropna(subset=[phase+"_raw"])
        vals=selected[phase+"_raw"]
        means=selected.groupby("period")[phase+"_raw"].mean()
        phase_rows.append([*keys,phase,len(vals),vals.std(),means.get("S2",np.nan)-means.get("S1",np.nan)])
write(pd.DataFrame(phase_rows,columns=["path","v","a","direction","phase","usable_legs","sd_raw","S2_minus_S1_raw"]),"repeatability")
print(t.groupby(["status","selected"]).size().to_string(),len(attempts))
print(s[["native_frames","motion_host_interval_p95_ms","host_gap_max_ms","command_bracket_ms","clock_discrepancy_ms","following_max","min_raw","max_raw","peak_step_raw","pre_capture_s","post_capture_s"]].agg(["min","median","max","sum"]).T)
print(c.to_string(index=False))
