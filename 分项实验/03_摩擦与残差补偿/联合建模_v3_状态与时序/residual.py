"""Residual-only MLP and causal sequence models."""
from common import *


def mlp(t, labels, predict, seed):
    start = perf_counter()
    train = t.loc[labels]
    w = base.weights(train)
    # Keep the original 2nd normalization from the V2 shared MLP entry point.
    model = v2.train_mlp(train[EXPLICIT].to_numpy(), train.label_raw.to_numpy(), w, seed)
    train_s = perf_counter() - start
    data = t.loc[predict, EXPLICIT].to_numpy()
    pred = base.predict(model, data)
    start = perf_counter()
    for row in data[:512]:
        base.predict(model, row.reshape(1, -1))
    us = (perf_counter() - start) / min(len(data),512) * 1e6
    arrays = dict(zip(("W1","b1","W2","b2","W3","b3"),model[0]))
    arrays.update(features=np.asarray(EXPLICIT), x_mean=model[1], x_scale=model[2],
                  y_mean=model[3], y_scale=model[4])
    return pred, dict(parameters=sum(x.size for x in model[0]), train_s=train_s,
                      online_us_per_grid=us, updates=2400, sequence="sample", precision="float64"), arrays


def baseline_replay(t):
    train, test = split(t, "internal")
    mask = t.candidate.to_numpy()
    train, test = train & mask, test & mask
    held = t.loc[test]
    previous = pd.read_csv(V2 / "internal_predictions.csv.gz")
    accuracy = pd.read_csv(V2 / "internal_metrics.csv")
    frozen = np.load(V1 / "models/M2.npz", allow_pickle=False)
    x = (held[frozen["features"].tolist()].to_numpy() - frozen["x_mean"]) / frozen["x_scale"]
    weights = [frozen[k] for k in ("W1","b1","W2","b2","W3","b3")]
    m2 = held.m1_raw.to_numpy() + base.forward(x, weights)[2][:,0]*frozen["y_scale"] + frozen["y_mean"]
    old = pd.read_csv(V1 / "internal_predictions.csv.gz")[["trace","frame","M2"]]
    new = held[["trace","frame"]].assign(replayed=m2).merge(old, validate="one_to_one")
    assert len(old) == len(new) == len(held)
    m2_error = float(np.max(np.abs(new.replayed-new.M2)))
    assert m2_error < 1e-6
    out = score(held,m2,model="M2_frozen",fold="internal",seed=426)
    m2_metrics = pd.read_csv(V1/"evaluation.csv")
    m2_metrics = m2_metrics[m2_metrics.model.eq("M2")]
    check = pd.DataFrame(out).merge(m2_metrics,on=["cohort","phase","seed"],suffixes=("","_old"))
    assert np.nanmax(np.abs(check.rmse-check.rmse_old)) < 1e-6
    matches = []
    models = RESULTS / "baseline_parameters"
    models.mkdir(exist_ok=True)
    for seed in P["seeds"]:
        residual, _, params = mlp(t,train,test,seed)
        np.savez(models/f"shared_arrival_{seed}.npz",**params)
        pred = held.m1_raw.to_numpy()+residual
        item = score(held,pred,model="T0_replay",fold="internal",seed=seed)
        out.extend(item)
        ref = accuracy[accuracy.model.eq("shared_arrival")]
        scored = pd.DataFrame(item).merge(ref,on=["cohort","phase","seed"],suffixes=("","_old"))
        difference = float(np.nanmax(np.abs(scored.rmse-scored.rmse_old)))
        assert difference < 1e-6
        row = dict(seed=seed, max_abs_metric_rmse_delta=difference)
        if seed == 426:
            check = held[["trace","frame"]].assign(replayed=pred).merge(
                previous[["trace","frame","shared_arrival"]],validate="one_to_one")
            assert len(check) == len(held)
            row["max_abs_point_delta"] = float((check.replayed-check.shared_arrival).abs().max())
            assert row["max_abs_point_delta"] < 1e-6
        matches.append(row)
    csv("baseline_replay.csv",out)
    save("baseline_verification.json",dict(rows=len(held), units=held.unit.nunique(),
                                         M2_max_abs_point_delta=m2_error, shared_arrival=matches))
    print("M2 and shared_arrival reproduced", matches, flush=True)

