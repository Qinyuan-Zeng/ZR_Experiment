"""Frozen engineering selection and independent E scoring."""
from replay import candidate_tables, metric, phases, grade, grid_trace, inspect, require, np, pd


def reconstruct(records):
    data,refs=[],[]
    for slot,session,w,legs in records:
        if slot["family"]=="SA":
            slices=[(0,legs[0]["hold_end"],[legs[0]]),(legs[0]["hold_start"],legs[-1]["hold_end"],[legs[-1]])]
        else:
            slices=[(0,legs[-1]["hold_end"],legs)]
        for i,(start,finish,selected) in enumerate(slices,1):
            t,r=grid_trace(w,slot,session,selected,start,finish,trace=f"{slot['id']}:{i}")
            data.append(t)
            refs.append(r)
    return pd.concat(data,ignore_index=True),pd.concat(refs,ignore_index=True)


def reversal(t,ref,slot,leg):
    """Watch centered displacement reference and two-grid sign persistence."""
    sign=leg["direction"]
    rv=ref.ref_v.to_numpy()
    reference=np.flatnonzero((t.frame.to_numpy()>=leg["settled"]-2000)&(sign*rv>=600))
    approach=np.flatnonzero((t.frame.to_numpy()<leg["settled"])&(sign*rv<=-600))
    require(len(reference)>=2 and len(approach)>=2,"Missing physical reversal evidence")
    # Bracket zero with the last nonzero native proxy on approach and first sustained return.
    g=np.flatnonzero((np.arange(len(rv))>approach[-1])&(sign*rv>0))
    require(len(g)>0,"No native reversal crossing")
    first_return=int(g[0])
    native=int(approach[-1]+np.argmin(np.abs(rv[approach[-1]:first_return+1])))
    rows=[]
    for name in ("T0","T1"):
        vel=t[name+"_v"].to_numpy()
        signed=sign*vel
        seen_before=np.count_nonzero((np.arange(len(rv))<native)&(signed<=-600))>=2
        seen_after=np.flatnonzero((np.arange(len(rv)-1)>=native-2)&
                                  (signed[:-1]>=600)&(signed[1:]>=600))
        # Restrict identification to the direction change, not a later leg or hold.
        seen_after=seen_after[seen_after<=native+20]
        delay=(seen_after[0]-native)*5 if seen_before and len(seen_after) else None
        rows.append(dict(slot=slot,model=name,reference_frame=int(t.frame.iloc[native]),
                         reference_s=float(t.time_s.iloc[native]),hit=delay is not None,delay_ms=delay))
    return rows


def tables(t,names,group):
    rows=[]
    for key,g in group:
        for name in names:
            modes=("strict","native","extra","lost","common") if len(names)>1 else ("strict","extra")
            for scope in modes:
                for label,mask in phases(g):
                    valid=(g["common"] if scope=="common" else g[name+"_"+scope]).to_numpy()
                    bounded=mask&valid&np.isfinite(g[name].to_numpy())
                    stats=metric(g,name,bounded)
                    rows.append(dict(slice=key,model=name,support=scope,phase=label,
                                     expected=int(mask.sum()),coverage=float(bounded.sum()/mask.sum()) if mask.any() else None,
                                     output_rows=int(np.sum(mask&np.isfinite(g[name].to_numpy()))),
                                     fallback_rows=0,**stats))
    return pd.DataFrame(rows)


def decision(counts,delays,error,complete):
    n0=sum(v["T0"] for v in counts.values())
    n1=sum(v["T1"] for v in counts.values())
    gain=n0>0 and 2*n1<=n0 and all(v["T1"]<=v["T0"] for v in counts.values())
    timing_ok=all(v.get("T1") is not None and v["T1"]<=(10 if v.get("T0") is None else v["T0"]+10)
                  for v in delays.values())
    support=all(v["T0"]["rows"]>0 and v["T1"]["rows"]>0 for v in error.values())
    precision=support and all(v["T1"]["rmse"]-v["T0"]["rmse"]<=1 and
                              v["T1"]["p95"]-v["T0"]["p95"]<=2 for v in error.values())
    choice="T1" if gain and timing_ok and precision else "T0"
    passes=complete and support and all(v.get(choice) is not None for v in delays.values())
    return dict(choice=choice if passes else None,passes=bool(passes),totals=[n0,n1],counts=counts,
                delays=delays,common_precision=error,false_sign_gain=gain,reversal_delay_ok=timing_ok,
                precision_ok=bool(precision),requires_severe_anomaly_review=True)


def select(records):
    t,ref=reconstruct(records)
    t=candidate_tables(t,["T0","T1"])
    sa=t.family.eq("SA")
    h=t.family.eq("H")
    events=[]
    for name in ("T0","T1"):
        # V3 false-sign detector runs on the whole S-A trace, regardless of scoring mask.
        ev,_,_=inspect(t.assign(history=""),ref,t[name+"_v"].to_numpy(),t[name+"_a"].to_numpy(),
                       sa.to_numpy(),model=name)
        events.extend(ev)
    timing=[]
    for slot,_,_,legs in records:
        if slot["family"]=="SB":
            mask=t.unit.eq(slot["id"])
            timing.extend(reversal(t.loc[mask],ref.loc[mask],slot["id"],legs[1]))
    cell={slot["id"]:(slot["speed"][0],slot["dec"][0]) for slot,_,_,_ in records if slot["family"]=="SA"}
    counts={str(k):{n:sum(x["model"]==n and cell[x["unit"]]==k for x in events) for n in ("T0","T1")}
            for k in sorted(set(cell.values()))}
    delays={slot:{x["model"]:x["delay_ms"] for x in timing if x["slot"]==slot} for slot in ("SB-1","SB-2")}
    error={}
    for label,mask in (("SA_loading",sa&t.phase.isin(["accelerate","cruise","brake"])),
                       ("SA_brake",sa&t.phase.eq("brake")),
                       ("H_2-6",h&t.hold_age_s.ge(2)&t.hold_age_s.lt(6))):
        error[label]={name:metric(t,name,mask&t.common) for name in ("T0","T1")}
    report=decision(counts,delays,error,len(records)==18 and len(timing)==4)
    groups=[("all",t),("SA",t.loc[sa]),("H",t.loc[h]),("SB",t.loc[t.family.eq("SB")])]
    groups.extend((f"SA-{k[0]}-D{k[1]}",t.loc[t.unit.isin([u for u,v in cell.items() if v==k])]) for k in sorted(set(cell.values())))
    groups.extend((f"H-{v}-{sg}",g) for (v,sg),g in t.loc[h&t.phase.eq("hold")].groupby(["speed","direction"]))
    groups.extend((f"H-trial:{u}",g) for u,g in t.loc[h].groupby("unit"))
    return report,tables(t,["T0","T1"],groups),t,pd.DataFrame(events),pd.DataFrame(timing)


def e_score(records,name):
    t,ref=reconstruct(records)
    t=candidate_tables(t,[name])
    events,_,_=inspect(t.assign(history=""),ref,t[name+"_v"].to_numpy(),t[name+"_a"].to_numpy(),
                      np.ones(len(t),bool),model=name)
    switches=[]
    for t_id,g in t.groupby("unit"):
        sel=g.transition_100ms.to_numpy()
        yp=g[name].to_numpy()
        good=g[name+"_strict"].to_numpy()
        delta=np.abs(np.diff(yp))
        pair=sel[1:]&sel[:-1]&good[1:]&good[:-1]&np.isfinite(delta)
        switches.append(dict(slot=t_id,expected=int(sel.sum()),strict=int((sel&good).sum()),
                             strict_adjacent_pairs=int(pair.sum()),
                             max_strict_jump_raw=float(delta[pair].max()) if pair.any() else None,
                             false_sign_events=sum(ev["unit"]==t_id for ev in events)))
    groups=[("all",t),*((f"session:{s}",g) for s,g in t.groupby("session")),
        *((f"template:{k}",g) for k,g in t.groupby("family")),
        *((f"template:{s}:{k}",g) for (s,k),g in t.groupby(["session","family"])),
        *((f"direction:{k}",g) for k,g in t.groupby("direction") if k),
        *((f"speed:{v}:dir:{sg}",g) for (v,sg),g in t.groupby(["speed","direction"]) if sg),
        *((f"slot:{k}",g) for k,g in t.groupby("unit"))]
    result=tables(t,[name],groups)
    required=result[result.support.eq("strict") & result.phase.isin(["loading","hold"]) &
             result.slice.str.startswith(("all","session:","template:"))].copy()
    ranked=required[required.slice.eq("all")|required.slice.str.startswith("session:")].copy()
    ranked["grade"]=[grade(r.phase,r.rmse,r.p95) for r in ranked.itertuples()]
    overall="insufficient" if (required.rows==0).any() else max(ranked.grade,key=["A","B","C","insufficient"].index)
    peaks=[]
    for r in result[result.support.eq("strict")].itertuples():
        if r.rows:
            w=t[t.trace.eq(r.peak_trace)&(t.time_s-r.peak_s).abs().le(.100000001)].copy()
            w["peak_scope"]=r.slice+":"+r.phase
            peaks.append(w)
    worst=[]
    for (speed,direction,unit),g in t[t.direction.ne(0)&t[name+"_strict"]].groupby(["speed","direction","unit"]):
        worst.append(dict(speed=speed,direction=int(direction),unit=unit,
                          **metric(g,name,np.ones(len(g),bool))))
    return (dict(grade=overall,provisional_purpose_review=True,scored_candidate=name),
            ranked,result,t,pd.concat(peaks,ignore_index=True) if peaks else pd.DataFrame(),pd.DataFrame(worst).sort_values("max_abs_raw",ascending=False).groupby(
                ["speed","direction"],as_index=False).head(1) if worst else pd.DataFrame(),pd.DataFrame(switches))
