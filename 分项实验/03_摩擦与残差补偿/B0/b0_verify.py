"""Recheck source integrity and B0 causal/data-domain invariants independently."""
import gzip
import json
from xml.etree import ElementTree

import numpy as np
import pandas as pd

from b0_sources import OUT, REPO, Run, baseline, sha
from b0_states import reconstruct


def future_invariance():
    ip,alpha,f1,grid=baseline()
    t=np.arange(0,.25,.005)
    status=np.full(len(t),1536,dtype=np.int64)
    x=15000+10000*t+20000*t**2
    measured=np.full(len(t),400.)
    stage=np.full(len(t),"scan")
    good=np.ones(len(t),dtype=bool)
    before=Run(dict(direction=1),t,t+.0004,x,measured,np.full(len(t),.0004),status,stage,good,.005)
    altered_x=x.copy()
    altered_i=measured.copy()
    altered_x[t>.15]+=120
    altered_i[t>.15]-=70
    after=Run(dict(direction=1),t,t+.0004,altered_x,altered_i,np.full(len(t),.0004),status,stage,good,.005)
    a,b=(reconstruct(r,ip,alpha,f1,grid) for r in (before,after))
    cols=[c for c in a if c.startswith("causal_")]
    mask=a["t_available_s"].le(.15)&a["t_grid_s"].le(.15)
    pd.testing.assert_frame_equal(a.loc[mask,cols],b.loc[mask,cols])
    diff=a.loc[mask,"a_ref_pulse_s2"]-b.loc[mask,"a_ref_pulse_s2"]
    assert (diff.abs()>1).any()
    assert np.allclose(a.loc[mask,"a_causal_pulse_s2"].dropna(),40000,atol=1e-6)


def verify_samples():
    total=0
    columns=["t_grid_s","t_available_s","t_native_s","x_pulse","I_actual_raw",
             "ref_I_p_raw","ref_I_a_raw","ref_I_f_raw","ref_I_M0_raw","ref_I_mech_raw",
             "ref_r_raw","ref_mechanism_domain_valid","ref_zero_friction_hold","ref_f1_speed_valid",
             "causal_I_p_raw","causal_I_a_raw","causal_I_f_raw","causal_I_M0_raw","causal_I_mech_raw",
             "causal_r_raw","causal_mechanism_domain_valid","causal_zero_friction_hold","causal_f1_speed_valid"]
    for q in pd.read_csv(OUT/"mechanism_samples.csv.gz",usecols=columns,chunksize=100_000):
        total+=len(q)
        assert (q["t_native_s"]<=q["t_available_s"]+1e-8).all()
        assert (q["t_available_s"]<=q["t_grid_s"]+1e-8).all()
        for prefix in ("ref","causal"):
            r=q.loc[q[prefix+"_mechanism_domain_valid"]]
            assert q.loc[~q[prefix+"_mechanism_domain_valid"],prefix+"_I_mech_raw"].isna().all()
            assert r["x_pulse"].between(1000,31000).all()
            moving=~r[prefix+"_zero_friction_hold"]
            assert r.loc[moving,"x_pulse"].between(10000,22000).all()
            assert r.loc[moving,prefix+"_f1_speed_valid"].all()
            assert np.allclose(r.loc[~moving,prefix+"_I_f_raw"],0,atol=1e-8)
            assert np.allclose(r[prefix+"_I_mech_raw"],r[prefix+"_I_p_raw"]+r[prefix+"_I_a_raw"]+r[prefix+"_I_f_raw"],
                               rtol=1e-7,atol=2e-6)
            assert np.allclose(r[prefix+"_r_raw"],r["I_actual_raw"]-r[prefix+"_I_mech_raw"],rtol=1e-7,atol=2e-6)
    return total


def main():
    future_invariance()
    src=pd.read_csv(OUT/"source_files.csv")
    for r in src.itertuples(index=False):
        assert sha(REPO/r.path)==r.sha256,r.path
    inv=pd.read_csv(OUT/"run_inventory.csv",low_memory=False)
    assert inv["run_id"].is_unique and inv["final_E"].eq(False).all()
    alias=pd.read_csv(OUT/"source_aliases.csv")
    assert alias.loc[alias["analysis"].eq("S2_C_integral"),"source_run_id"].nunique()==160
    assert alias["additional_independent_runs"].eq(0).all()
    gt=pd.read_csv(OUT/"gap_matrix.csv")
    assert gt.groupby("stage").size().to_dict()=={"B1":32,"B2":12,"bridge":4}
    assert gt["accepted_substitute_pairs"].le(gt["historical_exact_pairs"]).all()
    assert gt["accepted_substitute_pairs"].add(gt["pairs_to_acquire"]).eq(gt["planned_pairs"]).all()
    pts=verify_samples()
    assert pts==int(inv["grid_rows"].sum())
    figure_files=sorted((OUT/"figures").glob("*.svg"))
    assert len(figure_files)==4
    for f in figure_files:
        assert ElementTree.parse(f).getroot().tag.endswith("svg")
    for name in ("B0_summary.md","latency_and_causality.md"):
        assert len((OUT/name).read_text(encoding="utf-8"))>2000
    summary=dict(status="passed",source_hashes_verified=len(src),inventory_rows=len(inv),
                 timestamp_model_and_mask_rows_verified=pts,figures_verified=len(figure_files),
                 prefix_future_invariance=True,stage2_C_duplicate_source_legs=160,eligible_E_rows=0)
    (OUT/"verification.json").write_text(json.dumps(summary,indent=2)+"\n",encoding="utf-8")
    print(summary)


if __name__=="__main__":
    main()
