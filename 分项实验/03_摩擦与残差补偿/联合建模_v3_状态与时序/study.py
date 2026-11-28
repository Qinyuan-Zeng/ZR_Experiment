"""Run the locked baseline, A, B, C, then a diagnostic internal replay."""
import argparse
from common import *
from state import effective_r, load_grid, transformed
from residual import baseline_replay, mlp


def spec_name(spec):
    family, mode, hidden, window = spec
    return f"{family}_{mode}_{hidden}" + (f"_{window}" if mode == "window" else "")


def initial_specs():
    cfg = P["lstm"]
    return [(family,mode,cfg["default_hidden"],cfg["window_grids"] if mode == "window" else 0)
            for family in cfg["families"] for mode in cfg["modes"]]


def expanded_specs(spec):
    cfg = P["lstm"]
    family, mode, hidden, window = spec
    more = [(family,mode,h,window) for h in cfg["extension_hidden"]]
    if mode == "window":
        more.extend((family,mode,hidden,w) for w in cfg["extension_window_grids"])
    return more


def cached_model(t, train, test, seed, state_name, spec, fold):
    store = RESULTS / "fit"
    store.mkdir(exist_ok=True)
    label = "MLP" if spec is None else spec_name(spec)
    stem = store / f"{fold}__{state_name}__{label}__{seed}"
    out, info = stem.with_suffix(".npy"), stem.with_suffix(".json")
    source = ["protocol.json", "state.py", "common.py", "residual.py", "sequence.py", "study.py"]
    identity = dict(code={k:file_hash(HERE/k) for k in source},
                    data=read(V1/"training_config.json")["dataset_sha256"],
                    train=int(train.sum()), predict=int(test.sum()))
    if out.exists() and info.exists():
        result = read(info)
        assert result.pop("identity") == identity, stem
        return np.load(out,allow_pickle=False),result
    if spec is None:
        residual, result, arrays = mlp(t,train,test,seed)
    else:
        from sequence import fit
        residual, result, arrays = fit(t,train,test,seed,spec)
    yp = residual + t.loc[test,"m1_raw"].to_numpy()
    np.save(out,yp)
    np.savez(stem.with_suffix(".npz"),**arrays)
    info.write_text(json.dumps(dict(result,identity=identity),indent=2),encoding="utf8")
    print(fold,state_name,label,seed,round(result["train_s"],2),"s",flush=True)
    return yp,result


def combinations(grid, state, fold, configs, version):
    train, test = split(grid,fold)
    assert not set(grid.loc[train,"unit"]) & set(grid.loc[test,"unit"])
    valid = grid.candidate.to_numpy()
    rf = effective_r(grid,train)
    alt, alt_scope, state_us = transformed(grid,state,rf)
    rows = grid.loc[test]
    eval_rows = rows.candidate.to_numpy()
    both = eval_rows & alt_scope[test]
    predictions = rows[["cohort","unit","trace","frame","phase","segment","hold_age_s","candidate",
                        "current_raw","x_pulse","v_pulse_s","a_pulse_s2","ip_raw","ia_raw","if_raw",
                        "m1_raw","speed","direction","history","round","session"]].copy()
    predictions["v_alt"] = alt.loc[test,"v_pulse_s"].to_numpy()
    predictions["a_alt"] = alt.loc[test,"a_pulse_s2"].to_numpy()
    predictions["m1_alt"] = alt.loc[test,"m1_raw"].to_numpy()
    predictions["ia_alt"] = alt.loc[test,"ia_raw"].to_numpy()
    predictions["if_alt"] = alt.loc[test,"if_raw"].to_numpy()
    predictions["strict_common"] = both
    metrics, strict, timing, detail, per_unit = [],[],[],[],[]
    for name, altered, spec in configs:
        t = alt if altered else grid
        estimator = state if altered else "S0"
        for seed in P["seeds"]:
            yp,cost = cached_model(t,train&valid,test,seed,estimator,spec,fold)
            assert np.isfinite(yp[eval_rows]).all()
            tag = dict(model=name,fold=str(fold),seed=seed)
            metrics.extend(score(rows.loc[eval_rows],yp[eval_rows],**tag))
            strict.extend(score(rows.loc[both],yp[both],**tag))
            timing.append(dict(tag,**cost,state_estimation_us_per_grid=state_us if altered else 0.,
                               valid_original=int(eval_rows.sum()), strict_intersection=int(both.sum()),
                               missing_predictions=int((~np.isfinite(yp)&eval_rows).sum()),
                               watch_grids=len(rows), input_mode="5 ms encoder position only"))
            predictions[f"{name}_{seed}"] = yp
            if version != "B":
                detail.extend(strata(rows.loc[eval_rows],yp[eval_rows],**tag))
                eligible = rows.loc[eval_rows,["cohort","unit","phase"]].copy()
                eligible["error"] = rows.current_raw.to_numpy()[eval_rows]-yp[eval_rows]
                for (cohort,phase), subset in eligible.groupby(["cohort","phase"]):
                    unit = base.per_unit(subset.error.to_numpy(),subset.unit.to_numpy())
                    unit = unit.assign(cohort=cohort,phase=phase,**tag,
                                       max_abs=subset.groupby("unit").error.apply(lambda x:x.abs().max()).reindex(unit.unit).to_numpy())
                    per_unit.extend(unit.to_dict("records"))
    if version != "B":
        focus = predictions.candidate | predictions.unit.isin(["B1:R5-P2-V40-A600","B1:R5-P1-V2-A400"])
        predictions.loc[focus].to_csv(RESULTS / f"{version}_{fold}_predictions.csv.gz",
                                      index=False,compression=dict(method="gzip",compresslevel=3,mtime=0))
    return metrics, strict, timing, detail, per_unit


def collect(grid, state, folds, configs, version):
    tables = [[],[],[],[],[]]
    for fold in folds:
        result = combinations(grid,state,fold,configs,version)
        for dst,src in zip(tables,result):
            dst.extend(src)
    for name,rows in zip(["metrics","strict_metrics","cost","conditions","by_trial"],tables):
        if rows:
            csv(f"{version}_{name}.csv",rows)
    return pd.DataFrame(tables[0])


def b_stage():
    a = read(RESULTS/"A_choice.json")
    assert a["protocol"] == file_hash(HERE/"protocol.json")
    grid = load_grid()
    specs = initial_specs()
    def evaluate(menu):
        configs = [("MLP-V2",False,None),*((spec_name(s),False,s) for s in menu)]
        result = collect(grid,"S0",P["folds"],configs,"B")
        return rank(result,"MLP-V2","B")
    result = evaluate(specs)
    eligible = result[result.pass_error]
    if len(eligible):
        candidate = next(s for s in specs if spec_name(s)==eligible.model.iloc[0])
        specs += expanded_specs(candidate)
        result = evaluate(specs)
    csv("B_selection.csv",result)
    explore = result[result.model.ne("MLP-V2")].iloc[0].model
    accepted = result.loc[result.pass_error,"model"].tolist()
    by_name = {spec_name(s):s for s in specs}
    save("B_choice.json",dict(chosen=by_name[accepted[0]] if accepted else None,
                              exploratory=by_name[explore], protocol=file_hash(HERE/"protocol.json")))
    print(result.to_string(index=False),flush=True)


def matrix(a,b):
    state = a["chosen"] if a["chosen"] != "S0" else a["exploratory"]
    spec = b["chosen"] or b["exploratory"]
    configs = [("T0",False,None),("T1",True,None),("T2",False,spec),("T3",True,spec)]
    return state,configs


def c_stage():
    a,b = (read(RESULTS/f"{k}_choice.json") for k in ("A","B"))
    assert a["protocol"] == b["protocol"] == file_hash(HERE/"protocol.json")
    state, configs = matrix(a,b)
    opt_out = {"T1":a["chosen"] == "S0","T2":b["chosen"] is None,
               "T3":a["chosen"] == "S0" or b["chosen"] is None}
    metrics = collect(load_grid(),state,P["folds"],configs,"C_dev")
    result = rank(metrics,"T0","C")
    result["exploratory_branch"] = result.model.map(dict(T0=False,**opt_out))
    result["adoptable"] = result.pass_error & ~result.exploratory_branch
    csv("C_selection.csv",result)
    chosen = result.loc[result.adoptable,"model"].tolist()
    preferred = chosen[0] if chosen else "T0"
    summaries = ["A_selection.csv","B_selection.csv","C_selection.csv",
                 "A_choice.json","B_choice.json","C_dev_metrics.csv","input_hashes.json"]
    source = ["protocol.json","state.py","common.py","a_study.py","diagnostics.py",
              "residual.py","sequence.py","study.py"]
    save("selection_seal.json",dict(preferred=preferred, state=state, configs=configs,
                                   artifacts={k:file_hash(RESULTS/k) for k in summaries},
                                   code={k:file_hash(HERE/k) for k in source}))
    print(result.to_string(index=False),flush=True)
    print("C sealed recommendation:",preferred,flush=True)


def internal():
    from a_study import run
    seal = read(RESULTS/"selection_seal.json")
    for folder,key in ((RESULTS,"artifacts"),(HERE,"code")):
        assert all(file_hash(folder/k) == v for k,v in seal[key].items())
    assert inventory() == read(RESULTS/"input_hashes.json")
    grid = load_grid()
    ref = pd.read_pickle(RESULTS/"native_reference.pkl")
    run(grid,ref,["internal"],["S0",seal["state"],"S2"]
        if seal["state"] != "S2" else ["S0","S2"],"internal")
    collect(grid,seal["state"],["internal"],seal["configs"],"C_internal")
    assert inventory() == read(RESULTS/"input_hashes.json")


def main():
    cli=argparse.ArgumentParser(description=__doc__)
    cli.add_argument("stage",choices=["baseline","A","B","C","internal","report"])
    stage=cli.parse_args().stage
    if stage == "baseline":
        baseline_replay(load_grid())
    elif stage == "A":
        from a_study import develop
        develop()
    elif stage == "B":
        b_stage()
    elif stage == "C":
        c_stage()
    elif stage == "internal":
        internal()
    else:
        from report import main as report
        report()


if __name__ == "__main__":
    main()

