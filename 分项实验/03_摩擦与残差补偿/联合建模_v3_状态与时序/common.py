from pathlib import Path
from time import perf_counter
import hashlib
import json
import os
import sys

os.environ["OPENBLAS_NUM_THREADS"] = "1"
os.environ["OMP_NUM_THREADS"] = "1"
import numpy as np
import pandas as pd

HERE = Path(__file__).resolve().parent
V1 = HERE.parent / "联合建模_v1"
V2 = HERE.parent / "联合建模_v2_可行性"
sys.path[:0] = [str(V1), str(V2)]
import prepare_joint as prep
import compare as v2
import train_joint as base

P = json.loads((HERE / "protocol.json").read_text(encoding="utf8"))
DATA = V1 / "joint_samples.csv.gz"
RESULTS = HERE / "results"
RESULTS.mkdir(exist_ok=True)
XVA = base.FEATURES[:3]
EXPLICIT = base.FEATURES + ["arrival_speed", "travel"]
LOADING = ["accelerate", "cruise", "brake"]


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def save(name, value):
    (RESULTS / name).write_text(json.dumps(value, ensure_ascii=False, indent=2),
                                encoding="utf8")


def csv(name, table):
    pd.DataFrame(table).to_csv(RESULTS / name, index=False, encoding="utf-8-sig")


def groups(t):
    return t.groupby("trace", sort=False).indices.values()


def split(t, fold):
    dev = t.split.eq("dev").to_numpy()
    if fold == "internal":
        return dev, ~dev
    r = t["round"].eq(int(fold)).to_numpy()
    return dev & ~r, dev & r


def scopes(t):
    yield "ALL", np.ones(len(t), dtype=bool)
    for label in (*LOADING, "hold", "startup", "near_zero"):
        yield label, t.phase.eq(label).to_numpy()
    yield "loading", t.phase.isin(LOADING).to_numpy()
    yield "hold_low_speed", t.low_hold.to_numpy()
    yield "reverse_history", ((t.cohort == "C") & (t.history == "reverse")).to_numpy()
    for lo, hi in zip(P["hold_windows_s"][:-1], P["hold_windows_s"][1:]):
        yield f"hold_{lo:g}-{hi:g}s", ((t.hold_age_s >= lo) & (t.hold_age_s < hi)).to_numpy()


def score(t, yhat, **tags):
    err = t.current_raw.to_numpy() - yhat
    result = []
    for cohort in ("ALL", "B1", "C", "D"):
        scope = np.ones(len(t), bool) if cohort == "ALL" else t.cohort.eq(cohort).to_numpy()
        for phase, sel in scopes(t):
            mask = scope & sel
            result.append(dict(tags, cohort=cohort, phase=phase,
                               **base.evaluate(t.loc[mask], err[mask]),
                               max_abs=float(np.abs(err[mask]).max()) if mask.any() else np.nan))
    return result


def strata(t, prediction, **tags):
    cols = ["cohort", "session", "speed", "acceleration", "path", "history", "direction", "phase"]
    data = t[cols + ["unit", "current_raw"]].copy()
    data["err"] = data.current_raw.to_numpy() - prediction
    details = []
    for key, frame in data.groupby(cols, dropna=False):
        details.append(dict(tags, **dict(zip(cols, key)),
                            **base.evaluate(frame, frame.err.to_numpy()),
                            max_abs=frame.err.abs().max()))
    return details


def rank(metrics, baseline, family):
    phases = ["loading", "hold", "accelerate", "brake", "hold_low_speed"]
    summary = metrics[metrics.cohort.eq("ALL") & metrics.phase.isin(phases)]
    table = summary.pivot(index=["fold", "seed", "model"], columns="phase",
                          values="rmse")
    ref = table.xs(baseline, level="model")
    threshold = P["selection"]
    rows = []
    for model in table.index.get_level_values("model").unique():
        ratios = table.xs(model, level="model") / ref
        m = ratios.mean()
        f = ratios.groupby(level="fold").mean().max()
        hold_max = threshold["hold_ratio_state_max" if family == "A" else "hold_ratio_network_max"]
        pass_err = (m.loading <= 1 - threshold["min_loading_improvement"]
                    and f.loading <= threshold["fold_loading_ratio_max"]
                    and m.hold <= hold_max
                    and (family == "A" or f.hold <= threshold["fold_hold_ratio_network_max"])
                    and max(m.accelerate, m.brake) <= threshold["phase_ratio_max"]
                    and max(f.accelerate, f.brake) <= threshold["fold_phase_ratio_max"]
                    and m.hold_low_speed <= threshold["low_hold_ratio_max"])
        rows.append(dict(model=model, **m.to_dict(),
                         **{f"worst_fold_{k}": v for k, v in f.items()},
                         passing_pairs=int(((ratios.loading < 1) & (ratios.hold <= hold_max)).sum()),
                         mean_score=(m.loading + m.hold) / 2, pass_error=bool(pass_err)))
    return pd.DataFrame(rows).sort_values(["pass_error", "mean_score"], ascending=[False, True])


def file_hash(path):
    with path.open("rb") as fd:
        return hashlib.file_digest(fd, "sha256").hexdigest()


def input_paths():
    return [DATA, V1 / "models/M2.npz", V1 / "internal_predictions.csv.gz",
            V1 / "evaluation.csv", V1 / "sources.csv", V1 / "splits.csv",
            V1 / "training_config.json", V2 / "internal_predictions.csv.gz",
            V2 / "internal_metrics.csv", V2 / "development_metrics.csv",
            V2 / "candidate_config.json", V2 / "selection_seal.json",
            prep.POSITION, prep.ALPHA, prep.FRICTION,
            V1 / "prepare_joint.py", V1 / "train_joint.py", V2 / "compare.py"]


def inventory():
    return {str(p.relative_to(HERE.parent.parent)): file_hash(p) for p in input_paths()}

