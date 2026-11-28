from common import *
from state import estimate, effective_r, load_grid, mechanism, state_names
from diagnostics import inspect, native_reference


def coverage(t, original, native, selection, **tags):
    out = []
    for phase, scope in scopes(t):
        m = selection & scope
        old = original & m
        new = native & m
        out.append(dict(tags, phase=phase, grid=int(m.sum()), original=int(old.sum()),
                        mechanism_scope=int(new.sum()),
                        intersection=int((old & new).sum()),
                        lost=int((old & ~new).sum()), gained=int((new & ~old).sum())))
    return out


def run(t, ref, folds, names, version):
    metrics, strict_scores, supports, events, timing, kinematics, components = [], [], [], [], [], [], []
    r_values = {}
    for fold in folds:
        train, test = split(t, fold)
        r_floor = effective_r(t, train)
        r_values[fold] = r_floor
        valid = t.candidate.to_numpy()
        strict = test & valid
        predictions = {}
        for model in names:
            v, a, cost = estimate(t, model, r_floor)
            ia, fr, total, scope = mechanism(t, v, a, model == "S0")
            strict &= scope
            predictions[model] = total
            tags = dict(model=model, fold=str(fold), seed=426)
            use = test & valid
            assert np.isfinite(total[use]).all()
            metrics.extend(score(t.loc[use], total[use], **tags))
            supports.extend(coverage(t, valid, scope, test, **tags))
            change = t.ia_raw.to_numpy() - ia
            for phase, sel in scopes(t):
                m = use & sel
                if m.any():
                    components.append(dict(tags, phase=phase, count=int(m.sum()),
                                           ia_change_rms=np.sqrt(np.mean(change[m]**2)),
                                           fr_change_rms=np.sqrt(np.mean((t.if_raw.to_numpy()[m]-fr[m])**2)),
                                           position_only_rmse=base.evaluate(t.loc[m], (t.current_raw-t.ip_raw).to_numpy()[m])["rmse"],
                                           plus_inertia_rmse=base.evaluate(t.loc[m], (t.current_raw-t.ip_raw).to_numpy()[m]-ia[m])["rmse"],
                                           position_plus_friction_rmse=base.evaluate(t.loc[m], (t.current_raw-t.ip_raw).to_numpy()[m]-fr[m])["rmse"],
                                           causal_state_us_per_grid=cost,
                                           qj=float(model.split("_")[-1]) if model.startswith("S3") else np.nan,
                                           r_pulse2=r_floor*float(model.split("_")[1]) if model.startswith("S3") else np.nan))
            ev, ts, ks = inspect(t, ref, v, a, test, **tags)
            events.extend(ev)
            timing.extend(ts)
            kinematics.extend(ks)
            print("A", fold, model, flush=True)
        for model, prediction in predictions.items():
            strict_scores.extend(score(t.loc[strict], prediction[strict],
                                       model=model, fold=str(fold), seed=426))
    tables = dict(metrics=metrics, strict_metrics=strict_scores, coverage=supports, sign_events=events,
                  response=timing, kinematics=kinematics, components=components)
    for name, value in tables.items():
        csv(f"A_{version}_{name}.csv", value)
    save(f"A_{version}_R.json", r_values)
    return pd.DataFrame(metrics), pd.DataFrame(events), pd.DataFrame(timing)


def develop():
    assert not (RESULTS / "selection_seal.json").exists()
    save("input_hashes.json", inventory())
    t = load_grid()
    ref = native_reference(t)
    metrics, events, timing = run(t, ref, P["folds"], state_names(), "dev")
    result = rank(metrics, "S0", "A")
    cfg = P["selection"]
    threshold = timing[timing.model.eq("S0")]
    expected = threshold.true_reversal.sum()
    counts = events.groupby("model").size()
    for model in result.model:
        ind = result.model.eq(model)
        cur = timing[timing.model.eq(model)]
        delays = [cur[c].abs().quantile(.95) - threshold[c].abs().quantile(.95)
                  for c in ("rise_delay_ms", "fall_delay_ms")]
        reversal = float(cur.identified_reversal.sum()/expected) if expected else np.nan
        missing = sum((cur[f"{k}_expected"] & cur[f"{k}_delay_ms"].isna()).sum() for k in ("rise", "fall"))
        ref_missing = sum((threshold[f"{k}_expected"] & threshold[f"{k}_delay_ms"].isna()).sum()
                          for k in ("rise", "fall"))
        count = int(counts.get(model, 0))
        result.loc[ind, "sign_events"] = count
        result.loc[ind, "extra_p95_absolute_delay_ms"] = max(delays)
        result.loc[ind, "true_reversal_recall"] = reversal
        result.loc[ind, "missed_crossings"] = missing
        result.loc[ind, "pass_motion"] = (max(delays) <= cfg["extra_delay_ms_max"]
                                           and missing <= ref_missing
                                           and reversal >= cfg["min_true_reversal_recall"]
                                           and count <= counts.get("S0", 0)*cfg["new_false_reversal_ratio_max"])
    result["passes"] = result.pass_error & result.pass_motion.astype(bool)
    result = result.sort_values(["passes", "mean_score"], ascending=[False, True])
    csv("A_selection.csv", result)
    better = result.loc[result.passes, "model"].tolist()
    exploratory = result.loc[result.model.ne("S0")].sort_values("mean_score").model.iloc[0]
    save("A_choice.json", dict(chosen=better[0] if better else "S0", exploratory=exploratory,
                              protocol=file_hash(HERE / "protocol.json")))
    print(result.to_string(index=False), flush=True)


if __name__ == "__main__":
    develop()

