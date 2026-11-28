"""Additional trial-level diagnostics from the already fitted development run."""
from common import *
from state import estimate,load_grid,effective_r,mechanism,state_names
from report import svg


def b_details(grid):
    detail,units=[],[]
    for path in (RESULTS/"fit").glob("*__S0__*__*.npy"):
        fold,state,model,seed=path.stem.split("__")
        if not fold.isdigit():
            continue
        _,test=split(grid,int(fold))
        rows=grid.loc[test]
        keep=rows.candidate.to_numpy()
        yp=np.load(path,allow_pickle=False)[keep]
        rows=rows.loc[keep]
        name="MLP-V2" if model=="MLP" else model
        tags=dict(fold=fold,model=name,seed=int(seed))
        detail.extend(strata(rows,yp,**tags))
        fields=rows[["cohort","phase","unit"]].copy()
        fields["error"]=rows.current_raw.to_numpy()-yp
        for (cohort,phase),part in fields.groupby(["cohort","phase"]):
            unit=base.per_unit(part.error.to_numpy(),part.unit.to_numpy())
            unit=unit.assign(cohort=cohort,phase=phase,**tags,
                             max_abs=part.groupby("unit").error.apply(lambda x:x.abs().max()).reindex(unit.unit).to_numpy())
            units.extend(unit.to_dict("records"))
    csv("B_conditions.csv",detail)
    csv("B_by_trial.csv",units)


def motion_waveforms(t):
    ref=pd.read_pickle(RESULTS/"native_reference.pkl")
    a=pd.read_csv(RESULTS/"A_dev_response.csv")
    candidates=a[a.model.eq("S0")&a.true_reversal]
    trace=candidates.trace.iloc[0]
    g=t[t.trace.eq(trace)].reset_index(drop=True)
    r=ref[ref.trace.eq(trace)].reset_index(drop=True)
    v,a,_=estimate(g,"S2")
    motion=g.segment.isin(["approach","probe"]).to_numpy()
    idx=np.flatnonzero(motion)
    use=np.arange(max(0,idx[0]-14),min(len(g),idx[-1]+14))
    svg("real_reversal.svg",trace+" | approach and reverse probe",g.time_s.to_numpy()[use],
        [("Position [pulse]",[("Encoder",g.x_pulse.to_numpy()[use])]),
         ("Velocity [pulse/s]",[("Watch proxy",r.ref_v.to_numpy()[use]),
                                ("S0",g.v_pulse_s.to_numpy()[use]),("S2",v[use])]),
         ("Acceleration [pulse/s²]",[("Watch proxy",r.ref_a.to_numpy()[use]),
                                     ("S0",g.a_pulse_s2.to_numpy()[use]),("S2",a[use])])])
    x=g.x_pulse.to_numpy()
    from time import perf_counter
    s=[]
    _=prep.derivative(x,5)
    for _ in range(12):
        tick=perf_counter()
        prep.derivative(x,5)
        s.append((perf_counter()-tick)/len(x)*1e6)
    csv("A_S0_derivative_runtime.csv",[dict(mode="batch_5point",batch_rows=len(x),cpu_us_per_grid=np.median(s))])



def aligned_support(grid):
    from a_study import coverage
    v7 = np.zeros(len(grid))
    a7 = np.zeros(len(grid))
    pos = grid.x_pulse.to_numpy()
    for ix in groups(grid):
        v7[ix], a7[ix] = prep.derivative(pos[ix],7)
    _, fr7, _, _ = mechanism(grid,v7,a7)
    # Only valid F1 speeds are eligible for the second-window agreement check.
    sliding7 = (np.abs(v7)>=2000) & (np.abs(v7)<=40000)
    original = grid.candidate.to_numpy()
    still = grid.hold_zero.to_numpy()
    runs = []
    for fold in (*P["folds"], "internal"):
        train, test = split(grid,fold)
        floor = effective_r(grid,train)
        names = state_names() if fold != "internal" else ["S0","S2"]
        for name in names:
            v,a,_ = estimate(grid,name,floor)
            _, fr, _, valid = mechanism(grid,v,a,name=="S0")
            threshold = np.maximum(30000,.2*np.maximum(np.abs(a),np.abs(a7)))
            agrees = sliding7 & (np.abs(a-a7)<=threshold) & (np.abs(fr-fr7)<5)
            aligned = valid & (still | agrees)
            if name == "S0":
                assert np.array_equal(aligned[test],original[test])
            runs.extend(coverage(grid,original,aligned,test,model=name,fold=fold))
    csv("A_aligned_support.csv",runs)


def main():
    grid=load_grid()
    b_details(grid)
    motion_waveforms(grid)


if __name__=="__main__":
    main()

