"""Trial-level C/D diagnostics and source-linked vector figures."""
from itertools import combinations
from xml.sax.saxutils import escape

import numpy as np
import pandas as pd

from prepare_joint import OUT, csv
from train_joint import evaluate

WINDOWS = [(0,.2),(.2,1),(1,3),(3,4),(4,6)]
PHASE_LABELS = {"startup":"Startup","accelerate":"Acceleration","cruise":"Cruise","brake":"Deceleration","hold":"HOLD"}
COLOURS = {"M0":"#163d6a","M1":"#d86824","M2":"#208476","M2_no_direction":"#79549e","M2_no_time":"#727c86"}


def c_analysis(t):
    c = t.loc[(t.cohort=="C") & t.segment.isin(["probe","post"])].copy()
    keys = ["unit","round","direction","speed","history","dwell_ms","actual_dwell_ms","phase"]
    trial = c.groupby(keys,as_index=False).agg(rows=("frame","size"),supported=("candidate","sum"),
                                               residual_raw=("label_raw","mean"))
    csv("C_trial_phases.csv",trial)
    by = ["history","dwell_ms","direction","speed","phase"]
    csv("C_condition_summary.csv",trial.groupby(by,as_index=False).agg(
        trials=("unit","size"), supported_trials=("residual_raw","count"),
        actual_dwell_mean_ms=("actual_dwell_ms","mean"), actual_dwell_min_ms=("actual_dwell_ms","min"),
        actual_dwell_max_ms=("actual_dwell_ms","max"), bias_raw=("residual_raw","mean"),
        trial_sd_raw=("residual_raw","std")))
    diag = c.loc[c.phase.isin(["startup","near_zero"]) & c.ip_raw.notna()].copy()
    diag["position_only_error"] = diag.current_raw - diag.ip_raw
    q = diag.groupby(keys,as_index=False).agg(rows=("frame","size"),
        position_only_mean_raw=("position_only_error","mean"),
        position_only_peak_abs_raw=("position_only_error",lambda x:np.abs(x).max()))
    csv("C_start_nearzero_diagnostic.csv",q)
    c = c.loc[c.candidate].copy()
    c["x_bin"] = np.floor((c.x_pulse+50)/100).astype(int)
    c["v_bin"] = np.where(c.phase.eq("hold"),0,np.floor((c.v_pulse_s+250)/500)).astype(int)
    c["a_bin"] = np.where(c.phase.eq("hold"),0,np.floor((c.a_pulse_s2+15000)/30000)).astype(int)
    c["age_bin"] = np.where(c.phase.eq("hold"),np.floor(c.hold_age_s/.05),0).astype(int)
    bins = ["x_bin","v_bin","a_bin","age_bin"]
    pairs = []
    for (round_id,sign,speed,phase),g in c.groupby(["round","direction","speed","phase"]):
        for (left,l),(right,r) in combinations(g.groupby(["history","dwell_ms"]),2):
            if left[0]==right[0]:
                kind = "dwell_change"
            elif set((left[0],right[0]))=={"same","reverse"} and left[1]==right[1]:
                if abs(l.actual_dwell_ms.iloc[0]-r.actual_dwell_ms.iloc[0])>25:
                    continue
                kind = "direction_at_matched_dwell"
                if left[0]=="reverse":
                    left,right,l,r=right,left,r,l
            elif "direct" in (left[0],right[0]):
                kind = "direct_reference_confounded"
            else:
                continue
            common = l.groupby(bins).label_raw.mean().to_frame("left").join(
                r.groupby(bins).label_raw.mean().rename("right"),how="inner")
            if common.empty:
                continue
            pairs.append(dict(round=round_id,direction=sign,speed=speed,phase=phase,contrast=kind,
                              left_history=left[0],left_dwell_ms=left[1],right_history=right[0],right_dwell_ms=right[1],
                              left_actual_ms=l.actual_dwell_ms.iloc[0],right_actual_ms=r.actual_dwell_ms.iloc[0],
                              left_unit=l.unit.iloc[0],right_unit=r.unit.iloc[0],matched_cells=len(common),
                              right_minus_left_raw=(common.right-common.left).mean()))
    p = pd.DataFrame(pairs)
    csv("C_matched_trial_pairs.csv",p)
    keys = ["contrast","left_history","left_dwell_ms","right_history","right_dwell_ms","direction","speed","phase"]
    csv("C_matched_summary.csv",p.groupby(keys,as_index=False).agg(
        matched_repeats=("round","size"), mean_effect_raw=("right_minus_left_raw","mean"),
        repeat_sd_raw=("right_minus_left_raw","std"),mean_shared_cells=("matched_cells","mean")))


def d_analysis(t):
    d = t.loc[(t.cohort=="D") & t.phase.eq("hold")].copy()
    d["begin_s"] = np.nan
    d["end_s"] = np.nan
    for lo,hi in WINDOWS:
        use = d.hold_age_s.ge(lo)&d.hold_age_s.lt(hi)
        d.loc[use,["begin_s","end_s"]] = lo,hi
    d["target_pulse"] = d.unit.str.split("-").str[-2].astype(int)
    official = pd.read_csv(OUT.parent/"D/results/Z-D-HOLD-01/hold_windows.csv")
    assert len(official)==90 and official.valid.all()
    by = ["target_pulse","begin_s","end_s"]
    pairs = official.pivot(index=["repeat",*by],columns="direction",values="mean_raw").reset_index()
    pairs = pairs.rename(columns={1:"positive_raw",-1:"negative_raw"})
    pairs["direction_difference_raw"] = pairs.positive_raw-pairs.negative_raw
    pairs["direction_center_raw"] = (pairs.positive_raw+pairs.negative_raw)/2
    csv("D_hold_trial_pairs.csv",pairs)
    stats = pairs.groupby(by,as_index=False).agg(
        paired_repeats=("repeat","size"),direction_gap_raw=("direction_difference_raw","mean"),
        gap_sd_raw=("direction_difference_raw","std"),center_raw=("direction_center_raw","mean"),
        center_sd_raw=("direction_center_raw","std"))
    csv("D_hold_summary.csv",stats)
    pred = pd.read_csv(OUT/"internal_predictions.csv.gz")
    pred = pred.loc[(pred.cohort=="D")&pred.phase.eq("hold")].merge(
        d[["trace","frame","begin_s","end_s"]],on=["trace","frame"],validate="one_to_one")
    out = []
    for (lo,hi),g in pred.groupby(["begin_s","end_s"]):
        for model in ("M0","M1","M2","M2_no_direction","M2_no_time","M2_no_both"):
            out.append(dict(begin_s=lo,end_s=hi,model=model,
                            **evaluate(g,(g.current_raw-g[model]).to_numpy())))
    csv("D_hold_ablation_by_age.csv",pd.DataFrame(out))
    return stats


def subgroup_analysis(t):
    pred = pd.read_csv(OUT/"internal_predictions.csv.gz")
    meta = t[["trace","frame","direction","speed","path","history","dwell_ms"]]
    pred = pred.merge(meta,on=["trace","frame"],validate="one_to_one")
    rows = []
    for keys,g in pred.groupby(["cohort","phase","direction","speed"],dropna=False):
        for name in ("M0","M1","M2","M2_no_direction","M2_no_time","M2_no_both"):
            rows.append(dict(zip(["cohort","phase","direction","speed"],keys),model=name,
                             **evaluate(g,(g.current_raw-g[name]).to_numpy())))
    csv("evaluation_subgroups.csv",pd.DataFrame(rows))
    e = pd.read_csv(OUT/"evaluation_by_trial.csv")
    p = e.pivot(index=["cohort","phase","unit"],columns="model",values="mse").dropna()
    p["M2_minus_M1_rmse"] = np.sqrt(p.M2)-np.sqrt(p.M1)
    p["M2_worse"] = p.M2_minus_M1_rmse>0
    stats = p.groupby(["cohort","phase"],as_index=False).agg(
        units=("M2_worse","size"),units_worse=("M2_worse","sum"),
        median_delta_raw=("M2_minus_M1_rmse","median"),
        max_regression_raw=("M2_minus_M1_rmse","max"))
    csv("paired_regression_check.csv",stats)


def svg_start(w,h):
    return [f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" viewBox="0 0 {w} {h}">',
            f'<rect width="{w}" height="{h}" fill="white"/>',
            '<g font-family="Times New Roman, SimSun, serif" font-size="16" fill="#202020">']


def label(s,x,y,value,anchor="start",extras=""):
    s.append(f'<text x="{x:.2f}" y="{y:.2f}" text-anchor="{anchor}" {extras}>{escape(str(value))}</text>')


def line(s,x1,y1,x2,y2,colour="#222",width=1):
    s.append(f'<line x1="{x1:.2f}" y1="{y1:.2f}" x2="{x2:.2f}" y2="{y2:.2f}" stroke="{colour}" stroke-width="{width}"/>')


def finish(s,name):
    (OUT/name).write_text("\n".join([*s,"</g>","</svg>"]),encoding="utf-8")


def model_plot(ev):
    ev = ev.loc[ev.cohort.eq("ALL")&ev.model.isin(COLOURS)&ev.phase.isin(PHASE_LABELS)&ev.rows.gt(0)]
    phases = [p for p in PHASE_LABELS if p in set(ev.phase)]
    s = svg_start(1000,475)
    for panel,(metric,title) in enumerate((("rmse","RMSE (raw)"),("p95","Absolute error P95 (raw)"))):
        x0,y0,w,h = 155+panel*490,45,316,330
        top = np.ceil(ev[metric].max()/25)*25
        for tick in np.linspace(0,top,5):
            xp = x0+w*tick/top
            line(s,xp,y0,xp,y0+h,"#dddddd")
            line(s,xp,y0+h,xp,y0+h-5)
            label(s,xp,y0+h+22,f"{tick:g}","middle")
        for j,phase in enumerate(phases):
            y = y0+14+j*h/len(phases)
            label(s,x0-9,y+17,PHASE_LABELS[phase],"end")
            for k,(model,col) in enumerate(COLOURS.items()):
                row = ev.loc[ev.phase.eq(phase)&ev.model.eq(model),metric]
                if len(row):
                    s.append(f'<rect x="{x0}" y="{y+8*k:.1f}" width="{w*row.iloc[0]/top:.2f}" height="6.7" fill="{col}"/>')
        s.append(f'<rect x="{x0}" y="{y0}" width="{w}" height="{h}" fill="none" stroke="#222"/>')
        label(s,x0+w/2,y0+h+46,title,"middle")
        label(s,x0,26,f"({chr(97+panel)})")
    names = ["M0","M1","M2","No direction","No stop time"]
    for i,((_,col),name) in enumerate(zip(COLOURS.items(),names)):
        x=197+135*i
        line(s,x,449,x+22,449,col,5)
        label(s,x+27,454,name)
    finish(s,"internal_stage_errors.svg")


def d_plot(data):
    s = svg_start(770,435)
    x0,y0,w,h = 93,30,633,330
    sx=lambda x:x0+w*x/6
    sy=lambda y:y0+h*(1-y/160)
    for y in (0,40,80,120,160):
        line(s,x0,sy(y),x0+w,sy(y),"#ddd")
        line(s,x0,sy(y),x0+5,sy(y))
        label(s,x0-10,sy(y)+5,y,"end")
    for x in range(7):
        line(s,sx(x),y0+h,sx(x),y0+h-5)
        label(s,sx(x),y0+h+22,x,"middle")
    for j,(target,g) in enumerate(data.groupby("target_pulse")):
        c=["#163d6a","#d86824","#208476"][j]
        xx=(g.begin_s+g.end_s)/2
        yy=g.direction_gap_raw
        points=" ".join(f"{sx(x):.2f},{sy(y):.2f}" for x,y in zip(xx,yy))
        dash=["none","7,4","2,4"][j]
        s.append(f'<polyline points="{points}" fill="none" stroke="{c}" stroke-width="2" stroke-dasharray="{dash}"/>')
        for x,y,dy in zip(xx,yy,g.gap_sd_raw):
            line(s,sx(x),sy(y-dy),sx(x),sy(y+dy),c)
            s.append(f'<circle cx="{sx(x):.2f}" cy="{sy(y):.2f}" r="3.5" fill="{c}"/>')
        line(s,476+75*j,52,491+75*j,52,c,3)
        label(s,495+75*j,57,f"{target/1000:g} mm")
    s.append(f'<rect x="{x0}" y="{y0}" width="{w}" height="{h}" fill="none" stroke="#222"/>')
    label(s,x0+w/2,412,"Time from arrival confirmation (s)","middle")
    label(s,25,195,"Positive minus negative (raw)","middle",'transform="rotate(-90 25 195)"')
    finish(s,"D_direction_decay.svg")


if __name__=="__main__":
    table = pd.read_csv(OUT/"joint_samples.csv.gz",low_memory=False)
    c_analysis(table)
    d_plot(d_analysis(table))
    subgroup_analysis(table)
    model_plot(pd.read_csv(OUT/"evaluation.csv"))
