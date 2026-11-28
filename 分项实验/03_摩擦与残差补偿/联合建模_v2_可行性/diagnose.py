"""Read the sealed comparison and identify weak support and trial-level regressions."""
import numpy as np
import pandas as pd

from compare import OUT, V1, csv, dataset, gate, sha

DYNAMIC = ["accelerate","cruise","brake"]
SEED = 426


def baseline(t):
    a = pd.read_csv(V1/"evaluation_by_trial.csv")
    models = a.pivot(index=["cohort","phase","unit","samples"],columns="model",values="mse").reset_index()
    models["m1_rmse"] = np.sqrt(models.M1)
    models["m2_rmse"] = np.sqrt(models.M2)
    models["delta_raw"] = models.m2_rmse-models.m1_rmse
    audit = pd.read_csv(V1/"paired_regression_check.csv")
    for row in audit.itertuples():
        mask=(models.cohort==row.cohort)&(models.phase==row.phase)
        assert (models.loc[mask,"delta_raw"]>0).sum()==row.units_worse
    v = t.loc[t.split.eq("internal")&t.candidate]
    by = ["cohort","phase","unit"]
    z = v.groupby(by).agg(speed=("speed","first"),acceleration=("acceleration","first"),
                           history=("history","first"),path=("path","first"),
                           mean_abs_jerk=("jerk",lambda x:np.abs(x).mean()),
                           mean_abs_v=("v_pulse_s",lambda x:np.abs(x).mean())).reset_index()
    merged = models.merge(z,on=by,validate="one_to_one")
    csv("baseline_trial_diagnostics.csv",merged.sort_values("m2_rmse",ascending=False))
    subgroup = pd.read_csv(V1/"evaluation_subgroups.csv")
    p = subgroup.pivot(index=["cohort","phase","direction","speed","rows","units"],columns="model",values="rmse").reset_index()
    p["m2_minus_m1"] = p.M2-p.M1
    csv("baseline_subgroup_regressions.csv",p.sort_values("m2_minus_m1",ascending=False))
    sel = t.loc[t.candidate&t.cohort.eq("B1")&t.phase.eq("cruise")].copy()
    trial = sel.groupby(["split","unit","path","speed","acceleration","direction"],as_index=False).agg(
        mean_m1_error=("label_raw","mean"))
    drift = trial.groupby(["path","speed","acceleration","direction","split"]).mean_m1_error.mean().unstack("split").dropna()
    drift["internal_minus_dev_raw"] = drift["internal"]-drift["dev"]
    csv("B1_cruise_session_shift.csv",drift.reset_index())
    return merged


def coverage(t):
    labels = t.groupby(["cohort","split","phase"],as_index=False).agg(
        rows=("unit","size"),units=("unit","nunique"),supported=("candidate","sum"),
        short_ramp=("short_ramp","sum"),near_zero=("near_zero","sum"))
    csv("phase_support.csv",labels)
    h = t.loc[t.phase.eq("hold")].copy()
    h["age_band"] = pd.cut(h.hold_age_s,[-.001,.2,1,3,6.1],labels=["0-.2",".2-1","1-3","3-6"])
    keys = ["cohort","split","low_hold","age_band"]
    out = h.groupby(keys,observed=True,as_index=False).agg(
        rows=("unit","size"),eligible=("candidate","sum"),units=("unit","nunique"))
    csv("HOLD_support_by_speed_age.csv",out)
    v = t.loc[t.candidate].copy()
    v["soft_hold_gate"] = gate(v,"soft")
    v["hard_hold_gate"] = gate(v,"hard")
    csv("gate_diagnostics.csv",v.groupby(["cohort","split","phase"],as_index=False).agg(
        eligible=("unit","size"),mean_soft_gate=("soft_hold_gate","mean"),
        hard_hold_share=("hard_hold_gate","mean")))


def sign_sensitivity(t):
    pred = pd.read_csv(OUT/"internal_predictions.csv.gz")
    b = t.merge(pred[["trace","frame","M2_frozen","shared_v1","shared_arrival","semi_hold"]],
                on=["trace","frame"],validate="one_to_one")
    b = b.loc[b.cohort.eq("B1")&b.phase.isin(DYNAMIC)].copy()
    b["sign_mismatch"] = b.v_pulse_s*b.direction<0
    cols = ["unit","trace","phase","direction","frame","card_tick_raw","x_pulse","v_pulse_s",
            "a_pulse_s2","current_raw","m1_raw","M2_frozen","shared_arrival"]
    wrong = b.loc[b.sign_mismatch,cols].copy()
    source = pd.read_csv(V1/"sources.csv").set_index("trace")
    evidence = []
    for row in wrong.itertuples():
        raw = pd.read_csv(V1.parent/source.loc[row.trace,"raw"],
                          usecols=["frame","encoder_position_raw"]).set_index("frame")
        x = raw.encoder_position_raw
        evidence.append((x.loc[row.frame]-x.loc[row.frame-100],x.loc[row.frame+100]-x.loc[row.frame],
                         x.loc[row.frame-400:row.frame].min(),x.loc[row.frame-400:row.frame].max()))
    cols = ["backward_5ms_delta_pulse","next_5ms_delta_pulse_diagnostic","past_20ms_min","past_20ms_max"]
    wrong[cols] = pd.DataFrame(evidence,index=wrong.index,columns=cols)
    csv("velocity_sign_diagnostics.csv",wrong)
    brake = b.loc[b.phase.eq("brake")]
    rows = []
    for model in ("m1_raw","M2_frozen","shared_v1","shared_arrival","semi_hold"):
        for label,v in (("original",brake),("exclude_sign_disagreement_diagnostic",brake.loc[~brake.sign_mismatch])):
            err = (v.current_raw-v[model])**2
            rmse = np.sqrt(v.assign(err=err).groupby("unit").err.mean().mean())
            rows.append(dict(model=model,selection=label,rows=len(v),units=v.unit.nunique(),rmse=rmse))
    csv("brake_sensitivity.csv",pd.DataFrame(rows))


def comparison():
    r = pd.read_csv(OUT/"internal_metrics.csv")
    plans = pd.read_csv(OUT/"development_selection.csv")
    g = r.loc[(r.cohort=="ALL") & r.phase.isin(["ALL","loading","accelerate","cruise","brake","hold","hold_low_speed"])]
    result = g.groupby(["model","phase"],as_index=False).agg(rmse_mean=("rmse","mean"),
                    rmse_min=("rmse","min"),rmse_max=("rmse","max"),mae_mean=("mae","mean"),
                    p95_mean=("p95","mean"),bias_mean=("bias","mean"),units=("units","first"),
                    rows=("rows","first"))
    csv("候选模型对照.csv",result.merge(plans[["model","score","passes"]],on="model",how="left"))
    hold = r.query("phase in ['hold','hold_low_speed']").groupby(
        ["cohort","model","phase"],as_index=False).agg(rmse=("rmse","mean"),p95=("p95","mean"),
                                                     rows=("rows","first"),units=("units","first"))
    csv("HOLD_cohort_comparison.csv",hold)


if __name__=="__main__":
    t=dataset()
    baseline(t)
    coverage(t)
    comparison()
    sign_sensitivity(t)
