"""Offline stage 3 B0 audit. Run bundled Python on this file from any directory."""
from collections import Counter
from datetime import date
import gzip
import json

import numpy as np
import pandas as pd

from b0_sources import (OUT, REPO, S1, S2, S3, FRICTION, POSITION, ALPHA, PLAN_NAME, PLAN_SHA,
                        EXT_B, B_MANIFEST, D_MANIFEST, sha, relative, baseline, formal_acceptance, watch_membership,
                        host_runs, watch_runs, load)
from b0_states import DT, reconstruct, longest_observed, lag_audit

MOTION = ("acceleration","deceleration","steady","near_zero")
FEATURE_PHASES = ("acceleration","deceleration","steady","hold","pre_hold")
METRICS = ("ref_r_M0_raw","ref_r_raw","causal_r_M0_raw","causal_r_raw","ref_r_F1_position_diagnostic_raw")
COMPARISONS = ("ref_r_M0_common_raw","ref_r_M1_common_raw","causal_r_M0_common_raw",
               "causal_r_M1_common_raw","ref_r_M1_dual_common_raw","causal_r_M1_dual_common_raw")
STRATA = ["v_bin","a_bin","x_bin","phase","direction","field_session"]
COVER = ["cohort","dataset_role",*STRATA]
POS_EDGES = [-np.inf,1000,10000,22000,31000,np.inf]
VEL_EDGES = [-np.inf,2,5,10,15,20,30,40,np.inf]
ACC_EDGES = [-np.inf,-600000,-400000,-30000,30000,400000,600000,np.inf]


def write_csv(name, df):
    temporary=OUT/(name+".tmp")
    df.to_csv(temporary,index=False,encoding="utf-8-sig",float_format="%.9g",lineterminator="\n")
    temporary.replace(OUT/name)


def percentile(a,p):
    a=np.asarray(a)
    a=a[np.isfinite(a)]
    return np.percentile(a,p) if len(a) else np.nan


def condition_id(rec):
    speed=rec.get("nominal_speed_mm_s",np.nan)
    acc=rec.get("nominal_accel_mm_s2",np.nan)
    lo,hi=sorted((rec.get("commanded_start_mm",np.nan),rec.get("commanded_end_mm",np.nan)))
    return f'{rec["cohort"]}|{speed:g}|{acc:g}|{lo:g}:{hi:g}|{rec.get("execution_role","")}|{rec["direction"]:+d}'


def run_quality(record,run,df):
    raw_gaps=np.diff(run.t)*1000
    record.update(native_origin_s=float(run.t[0]),native_p50_ms=percentile(raw_gaps,50), native_p95_ms=percentile(raw_gaps,95),
                  native_max_ms=percentile(raw_gaps,100), nonmonotonic_steps=int((raw_gaps<=0).sum()),
                  gap_over_2p5_native=int((raw_gaps>run.nominal*2500).sum()),
                  bad_native_rows=int((~run.good).sum()), host_read_span_p95_ms=percentile(run.read_duration*1000,95),
                  grid_rows=len(df))
    if df.empty:
        return
    motion=df["phase"].isin(MOTION).to_numpy()
    time=df["t_grid_s"].to_numpy()
    segments=df["segment"].to_numpy()
    eligible=df["causal_mechanism_domain_valid"].to_numpy()
    native_segment=np.insert(np.cumsum((np.diff(run.t) <= 0) | (np.diff(run.t) > 2.5*run.nominal) |
                                      (~run.good[:-1]) | (~run.good[1:])),0,0)
    pre_mask=np.isin(run.stage,["before_trial","approach_start","watch_pre"]) & run.good
    post_mask=np.isin(run.stage,["hold","turnaround","watch_hold","trial_end"]) & run.good
    record.update(usable_ref_M0=int(df["ref_r_M0_raw"].notna().sum()),
                  usable_ref_M1=int(df["ref_r_raw"].notna().sum()),
                  usable_causal_M1=int(eligible.sum()),
                  usable_causal_dynamic=int((eligible&df["phase"].isin(("acceleration","deceleration")).to_numpy()).sum()),
                  samples_f1_above40=int(df["ref_f1_above_40"].sum()),
                  samples_f1_near_zero=int(df["ref_f1_near_zero_unresolved"].sum()),
                  samples_moving_outside_f1_x=int((motion&~df["ref_f1_position_valid"].to_numpy()).sum()),
                  pre_observed_s=longest_observed(pre_mask,run.t,native_segment),
                  post_observed_s=longest_observed(post_mask,run.t,native_segment),
                  pre_stable_s=longest_observed((df["phase"].eq("pre_hold")&df["ref_zero_friction_hold"]).to_numpy(),time,segments),
                  post_stable_s=longest_observed((df["phase"].eq("hold")&df["ref_zero_friction_hold"]).to_numpy(),time,segments),
                  moving_observed_s=longest_observed(motion,time,segments),
                  x_min_pulse=float(df["x_pulse"].min()),x_max_pulse=float(df["x_pulse"].max()),
                  v_min_pulse_s=float(df["v_ref_pulse_s"].min()),v_max_pulse_s=float(df["v_ref_pulse_s"].max()),
                  abs_motion_speed_p95_mm_s=percentile(np.abs(df.loc[motion,"v_ref_pulse_s"])/1000,95),
                  a_min_pulse_s2=float(df["a_ref_pulse_s2"].min()),a_max_pulse_s2=float(df["a_ref_pulse_s2"].max()),
                  grid_age_p95_ms=percentile(df["current_source_age_ms"],95),
                  contiguous_segments=int(df["segment"].nunique()),
                  condition=condition_id(record))
    for ph in ("acceleration","deceleration","steady","near_zero","hold"):
        record[f"grid_{ph}_n"]=int(df["phase"].eq(ph).sum())
    record["usable_for_model_development"] = bool(record["dataset_role"]=="train_candidate"
        and record["usable_causal_dynamic"]>=3)


def coverage_rows(df,record):
    sample=df.loc[df["v_ref_pulse_s"].notna() & df["frame_valid"],
                  ["phase","x_pulse","v_ref_pulse_s","a_ref_pulse_s2","ref_ip_position_valid",
                   "ref_f1_position_valid","ref_f1_speed_valid","ref_mechanism_domain_valid",
                   "causal_mechanism_domain_valid",*METRICS]].copy()
    if sample.empty:
        return sample
    sample["v_bin"]=pd.cut(sample["v_ref_pulse_s"].abs()/1000,VEL_EDGES,right=False)
    sample["a_bin"]=pd.cut(sample["a_ref_pulse_s2"],ACC_EDGES)
    sample["x_bin"]=pd.cut(sample["x_pulse"],POS_EDGES)
    for key in ("run_id","pair_id","direction","cohort","dataset_role","field_session","condition","raw_quality"):
        sample[key]=record.get(key,"")
    sample["x_bin_1mm"]=np.floor(sample["x_pulse"]/1000)
    sample["v_band"]=sample["v_ref_pulse_s"].abs()/1000
    sample["a_band"]=sample["a_ref_pulse_s2"].abs()/1000
    for field in (*COVER,"run_id","pair_id","condition"):
        sample[field]=sample[field].astype(str).astype("category")
    return sample.drop(columns=["x_pulse","v_ref_pulse_s","a_ref_pulse_s2"])


def group_metrics(data,fields,scope):
    # Per-run equal weight for bias/RMSE diagnostics, alongside ordinary per-grid summaries.
    result=[]
    for keys, frame in data.groupby(fields,observed=True,sort=False):
        if not isinstance(keys,tuple): keys=(keys,)
        labels=dict(zip(fields,keys))
        for metric in (*METRICS,*COMPARISONS):
            valid=frame.loc[frame[metric].notna(),["run_id","pair_id",metric]]
            if not len(valid):
                continue
            err=valid[metric].to_numpy()
            grouped=valid.groupby("run_id",observed=True)[metric]
            means=grouped.mean().to_numpy()
            squares=valid.assign(square=err**2).groupby("run_id",observed=True)["square"].mean().to_numpy()
            result.append(dict(scope=scope,**labels,metric=metric,
                               samples=len(valid),runs=len(means),pairs=valid["pair_id"].nunique(),
                               bias_raw=err.mean(),rmse_raw=np.sqrt(np.mean(err**2)),mae_raw=np.mean(np.abs(err)),
                               p95_abs_raw=np.percentile(np.abs(err),95),run_balanced_rmse_raw=np.sqrt(squares.mean()),
                               between_run_bias_sd_raw=np.std(means,ddof=1) if len(means)>1 else np.nan))
    return result


def repeatability(data):
    # Cross-run waveform agreement within condition, direction and phase on 1 mm bins.
    # Count each original recorded run once per bin. No fitting on independent E or NN training.
    selected=data[data["raw_quality"] & data["ref_r_raw"].notna()
                  & data["phase"].isin(FEATURE_PHASES)
                  & data["dataset_role"].isin(["train_candidate","hold_candidate"])]
    repeats=[]
    for (condition,phase), grp in selected.groupby(["condition","phase"],observed=True):
        by_run=grp.groupby("run_id",observed=True)["ref_r_raw"].mean()
        if len(by_run)<3 or grp["pair_id"].nunique()<3:
            continue
        bins=grp.pivot_table(index="run_id",columns="x_bin_1mm",values="ref_r_raw",aggfunc="mean",
                             observed=True)
        profiles=[]
        for i in range(len(bins)):
            for j in range(i+1,len(bins)):
                aa,bb=bins.iloc[[i,j]].dropna(axis=1).to_numpy()
                if len(aa)>=4 and np.std(aa)>.1 and np.std(bb)>.1:
                    profiles.append(np.corrcoef(aa,bb)[0,1])
        within=grp.groupby(["field_session","x_bin_1mm","run_id"],observed=True)["ref_r_raw"].mean().reset_index()
        session_curves=within.pivot_table(index="field_session",columns="x_bin_1mm",values="ref_r_raw",
                                         aggfunc="mean",observed=True)
        cross=[]
        for i in range(len(session_curves)):
            for j in range(i+1,len(session_curves)):
                aa,bb=session_curves.iloc[[i,j]].dropna(axis=1).to_numpy()
                if len(aa)>=4 and np.std(aa)>.1 and np.std(bb)>.1:
                    cross.append(np.corrcoef(aa,bb)[0,1])
        repeats.append(dict(condition=condition,cohort=str(grp["cohort"].iloc[0]),phase=phase,
                            distinct_runs=len(by_run),independent_pairs=grp["pair_id"].nunique(),
                            field_sessions=grp["field_session"].nunique(),split_groups=grp["split_group"].nunique(),
                            mean_run_bias_raw=by_run.mean(),between_run_bias_sd_raw=by_run.std(ddof=1),
                            rms_run_mean_raw=np.sqrt(np.mean(by_run.to_numpy()**2)),
                            trace_pair_count=len(profiles),trace_pearson_r_median=np.median(profiles) if profiles else np.nan,
                            between_session_curve_pairs=len(cross),between_session_pearson_r_median=np.median(cross) if cross else np.nan))
    return pd.DataFrame(repeats)


def planned_conditions():
    b1=[(2,400),(5,400),(10,400),(10,600)]+[(v,a) for v in (15,20,30,40) for a in (400,500,600)]
    groups=[
        ("B1",b1,[("P1",10,22),("P2",12,20)]),
        ("B2",[(v,a) for v in (60,80,100) for a in (500,600)],[("H1",3,29),("H2",4,28)]),
        ("bridge",[(40,a) for a in (500,600)],[("G1",3,29)]),
        ("bridge",[(60,a) for a in (500,600)],[("G2",10,22)]),
    ]
    for block,combos,paths in groups:
        for v,a in combos:
            for path,lo,hi in paths:
                yield block,path,lo,hi,v,a


def gaps(inventory, data):
    rows=[]
    quality=inventory[inventory["raw_quality"].eq(True)].copy()
    motions=data[data["phase"].isin(MOTION) & data["raw_quality"]]
    for block,path,lo,hi,v,a in planned_conditions():
        cand=quality[quality["nominal_speed_mm_s"].eq(v) & quality["nominal_accel_mm_s2"].eq(a)]
        start,end=cand["commanded_start_mm"],cand["commanded_end_mm"]
        same=((start-lo).abs().le(.010)&(end-hi).abs().le(.010)) | ((start-hi).abs().le(.010)&(end-lo).abs().le(.010))
        exact=cand[same].copy()
        exact_pairs=0
        reusable=Counter()
        for _,g in exact.groupby("pair_id"):
            actual_directions=set(g["direction"])
            if actual_directions!={-1,1} or len(g)!=2:
                continue
            exact_pairs+=1
            if (g["pre_stable_s"].ge(.9)&g["post_stable_s"].ge(.9)
                &g["abs_motion_speed_p95_mm_s"].ge(.95*v)&g["abs_motion_speed_p95_mm_s"].le(1.05*v)
                &g["moving_observed_s"].ge(.8*((hi-lo)/v+v/a))
                &g["phase_source"].isin(["C","AF","A0F"])
                &g["dataset_role"].eq("train_candidate")).all() and g["split_group"].nunique()==1:
                reusable[g["split_group"].iloc[0]]+=1
        direct=min(5,sum(min(n,3 if i==0 else 2) for i,n in enumerate(sorted(reusable.values(),reverse=True))))
        speed_overlap=motions["v_band"].between(max(0,v-max(.25,.05*v)),v+max(.25,.05*v))
        accelerate_overlap=motions["a_band"].between(.8*a,1.2*a)
        # Descriptive state overlap follows the analysis position, not commanded endpoints.
        inside=motions["sample_x_mm"].between(lo,hi)
        direct_states=motions[speed_overlap&inside]
        dynamic=motions[speed_overlap&inside&accelerate_overlap]
        need=4510+(v*1000)**2/(2*.99e6)
        clearance=min(lo*1000,37000-hi*1000)
        reason=("exact_path_direction_or_holds_missing" if len(direct_states)
                else "no_measured_state_coverage")
        if direct:
            reason="credited_complete_independent_pairs"
        rows.append(dict(stage=block,path=path,start_mm=lo,end_mm=hi,target_speed_mm_s=v,
                         target_accel_mm_s2=a,planned_pairs=5,historical_exact_legs=len(exact),
                         historical_exact_pairs=exact_pairs,eligible_field_groups=len(reusable),
                         accepted_substitute_pairs=direct,pairs_to_acquire=5-direct,legs_to_acquire=2*(5-direct),
                         partial_speed_position_runs=direct_states["run_id"].nunique(),
                         partial_speed_position_sessions=direct_states["field_session"].nunique(),
                         partial_joint_state_runs=dynamic["run_id"].nunique(),
                         partial_joint_state_positive_grid=int(dynamic["direction"].eq("1").sum()),
                         partial_joint_state_negative_grid=int(dynamic["direction"].eq("-1").sum()),
                         ramp_s=v/a,ideal_cruise_mm=(hi-lo)-v*v/a,
                         required_each_side_under_A_protection_pulse=need,
                         available_min_side_under_A_protection_pulse=clearance,
                         A_protection_screen="pass" if clearance>=need else "fails",
                         audit_reason=reason))
    return pd.DataFrame(rows)


def main():
    ip,alpha,f1,grid=baseline()
    accepted=formal_acceptance()
    memberships=watch_membership()
    sources={POSITION,ALPHA,FRICTION,FRICTION.with_name("protocol.json"),FRICTION.with_name("protocol.sha256"),
             S3/"results/formal_a/Z-AF-20261126-03/pairs.csv",
             S2/"results/c"/f"{EXT_B}_summary.json",S2/"results/c"/f"{EXT_B}_trajectories.csv",
             B_MANIFEST,D_MANIFEST,S2/"results/b/A1_B1_summary.json"}
    watches=[]
    records,stats,lags=[],[],[]
    groups=(host_runs(S1,"stage1",accepted,sources),watch_runs(memberships,sources,watches),
            host_runs(S3,"stage3",accepted,sources))
    file=OUT/"mechanism_samples.csv.gz"
    scratch=file.with_suffix(".tmp")
    first=True
    with scratch.open("wb") as binary, gzip.GzipFile(filename="",fileobj=binary,mode="wb",mtime=0,compresslevel=3) as gz:
        for group in groups:
            for record,run in group:
                records.append(record)
                if run is None:
                    continue
                frame=reconstruct(run,ip,alpha,f1,grid)
                run_quality(record,run,frame)
                if frame.empty:
                    continue
                record["model_version"]="position_B_primary_v1|Z-DP_20261126-095126_343_29884|Z-AF-20261126-03:F1"
                lag=lag_audit(frame,record) if record["raw_quality"] and record["dataset_role"]=="train_candidate" and record["cohort"]!="S1_B_primary" else []
                lags.extend(lag)
                sl=coverage_rows(frame,record)
                if not sl.empty:
                    sl["sample_x_mm"]=frame.loc[sl.index,"x_pulse"].to_numpy()/1000
                    sl["split_group"]=record["split_group"]
                    stats.append(sl)
                for key in reversed(("run_id","pair_id","session_id","field_session","split_group","cohort","direction",
                                     "dataset_role","raw_quality","model_version")):
                    frame.insert(0,key,record[key])
                data=frame.to_csv(index=False,header=first,lineterminator="\n",float_format="%.9g")
                gz.write(data.encode("utf-8"))
                first=False
            print(f"Source processed: {record['source']} ({len(records)} cumulative inventory rows)",flush=True)
    scratch.replace(file)
    inventory=pd.DataFrame(records)
    assert len(inventory)==inventory["run_id"].nunique()
    assert int(inventory["final_E"].eq(True).sum())==0
    for col in ("pre_stable_s","post_stable_s","abs_motion_speed_p95_mm_s","moving_observed_s"):
        if col not in inventory: inventory[col]=np.nan
    write_csv("run_inventory.csv",inventory)
    samples=pd.concat(stats,ignore_index=True)
    for c in (*COVER,"run_id","pair_id","condition","split_group"):
        samples[c]=samples[c].astype("category")
    state=samples.groupby(COVER,observed=True,sort=True).agg(
        grid_samples=("run_id","size"),unique_runs=("run_id","nunique"),unique_pairs=("pair_id","nunique"),
        ip_domain_samples=("ref_ip_position_valid","sum"),f1_position_samples=("ref_f1_position_valid","sum"),
        f1_sliding_speed_samples=("ref_f1_speed_valid","sum"),ref_M1_samples=("ref_mechanism_domain_valid","sum"),
        causal_M1_samples=("causal_mechanism_domain_valid","sum")).reset_index()
    write_csv("state_coverage.csv",state)
    publication=samples[samples["raw_quality"] & samples["phase"].ne("transfer")].copy()
    for scheme in ("ref","causal"):
        r0,r1=publication[scheme+"_r_M0_raw"],publication[scheme+"_r_raw"]
        mask=r0.notna()&r1.notna()
        publication[scheme+"_r_M0_common_raw"]=r0.where(mask)
        publication[scheme+"_r_M1_common_raw"]=r1.where(mask)
    dual=publication[["ref_r_raw","causal_r_raw"]].notna().all(axis=1)
    for scheme in ("ref","causal"):
        publication[scheme+"_r_M1_dual_common_raw"]=publication[scheme+"_r_raw"].where(dual)
    metrics=[]
    for keyset,scope in ((["cohort","dataset_role","phase"],"phase"),
                         (["cohort","dataset_role","phase","direction","field_session"],"session_direction"),
                         (["cohort","dataset_role","phase","v_bin","a_bin"],"speed_acceleration"),
                         (["cohort","dataset_role","phase","direction","x_bin"],"position_direction")):
        metrics.extend(group_metrics(publication,keyset,scope))
    write_csv("mechanism_residuals.csv",pd.DataFrame(metrics))
    write_csv("repeated_conditions.csv",repeatability(publication))
    write_csv("lag_sensitivity.csv",pd.DataFrame(lags))
    write_csv("gap_matrix.csv",gaps(inventory,samples))
    cdata=pd.read_csv(S2/"results/c"/f"{EXT_B}_trajectories.csv")
    cids=set(cdata["run_id"].astype(str))
    assert cids <= set(inventory["trial_id"])
    aliases=[dict(analysis="S2_C_integral",source_run_id="stage2:"+run,additional_independent_runs=0)
             for run in sorted(cids)]
    for r in inventory[inventory["duplicate_source"].fillna("").ne("")].to_dict("records"):
        aliases.append(dict(analysis="identical_raw_sha256",source_run_id=r["run_id"],
                            retained_source=r["duplicate_source"],additional_independent_runs=0))
    write_csv("source_aliases.csv",pd.DataFrame(aliases))
    all_src=sorted(p for p in sources if p.is_file())
    sidecars=set(watches)
    write_csv("source_files.csv",pd.DataFrame([
        dict(path=relative(p),sha256=sha(p),bytes=p.stat().st_size,
             file_role="sidecar" if p in sidecars else "primary_or_provenance") for p in all_src]))
    snapshot=dict(plan_name=PLAN_NAME,plan_sha256=PLAN_SHA,
                  analysis_period_s=DT,position_model_sha256=sha(POSITION),alpha_source_sha256=sha(ALPHA),
                  friction_model_sha256=sha(FRICTION),alpha_raw_per_pulse_s2=alpha,
                  position_table_semantic_sha256=ip["table_sha256"],
                  friction_coefficients=f1,Ip_position_pulse=ip["range_pulse"],
                  F1_position_pulse=[grid[0],grid[-1]],F1_sliding_speed_pulse_s=[2000,40000],
                  historical_runs=len(inventory),five_ms_samples=int(inventory["grid_rows"].sum()),
                  E_historical_samples_eligible=0)
    (OUT/"audit_provenance.json").write_text(json.dumps(snapshot,ensure_ascii=False,indent=2)+"\n",encoding="utf-8")
    print(inventory.groupby(["source","dataset_role"],dropna=False).size().to_string())
    print(f"{len(inventory)} inventory rows, {len(samples)} state rows, {len(state)} coverage cells, {len(metrics)} error strata")


if __name__=="__main__":
    main()
