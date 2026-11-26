"""Validate formal A runs and fit the two direction-specific steady friction baselines."""
import math

import numpy as np

from a0_replay import ROOT, MODEL, digest, longest_plateau, read_csv, require, write_csv
from a_dense_plan import WINDOW

GRID_STEP = 250
STOPS = "smooth_stop_dec_pulse_ms2"


def analysis_grid(start=WINDOW[0]):
    require(isinstance(start, int) and start % GRID_STEP == 0
            and WINDOW[0] <= start <= WINDOW[1] - 2*GRID_STEP,
            "Analysis start must be on the 250-pulse grid and leave at least three points")
    return np.arange(start, WINDOW[1] + 1, GRID_STEP)


def endpoints(p, direction):
    return (p["lo"], p["hi"]) if direction == 1 else (p["hi"], p["lo"])


def fields(path):
    return dict(line.split("=", 1) for line in path.read_text(encoding="utf-8-sig").splitlines() if "=" in line)


def checked_state(record, p):
    values = fields(record)
    position, smooth = float(values["position_pulse"]), float(values[STOPS])
    drive, error = float(values["controller_follow_limit_pulse"]), float(values["following_error_pulse"])
    follow = min(p["follow"], drive) if drive > 0 else p["follow"]
    require(values["ready"] == "1" and 0 <= position <= 32000 and p["safe_lo"] <= position <= p["safe_hi"]
            and abs(error) <= follow and smooth >= p["min_stop"], f"Axis state outside frozen protection: {record}")
    return position, smooth


def analyze(frozen, attempt, request, model):
    p = frozen["params"]
    grid = np.asarray(frozen["grid"])
    manifest = ROOT / "manifest"
    sessions = [(file, record) for file in manifest.glob("*.txt") if
                (record := fields(file)).get("protocol_id") == frozen["id"] and record.get("attempt_id") == attempt.name]
    require(len(sessions) == 1, "Expected one acquisition session for this attempt")
    meta_file, meta = sessions[0]
    session = meta_file.stem
    require(meta["session_id"] == session and meta["phase"] == "AF"
            and all(meta[k] == v for k, v in model["conditions"].items()), "Axis or signal configuration differs")
    for field, value in (("acc_mm_s2", p["accel"]/1000), ("transfer_mm_s", p["transfer"]),
                         ("safe_lo_pulse", p["safe_lo"]), ("safe_hi_pulse", p["safe_hi"]),
                         ("reserve_pulse", p["reserve"]), ("requested_follow_pulse", p["follow"]),
                         ("period_s", .005), ("hold_s", .25), ("minimum_smooth_stop_pulse_ms2", p["min_stop"]),
                         ("grid_lo_pulse", grid[0]), ("grid_hi_pulse", grid[-1])):
        require(math.isclose(float(meta[field]), value, rel_tol=1e-10), f"Recorded {field} differs from freeze")
    before, after = attempt/"pre.txt", attempt/"post.txt"
    _, pre_stop = checked_state(before, p)
    _, post_stop = checked_state(after, p)
    require(fields(before)["controller_follow_limit_pulse"] == fields(after)["controller_follow_limit_pulse"]
            and pre_stop == post_stop == float(meta[STOPS]), "Controller protection changed during batch")
    raw = ROOT/"data/raw"/(session+".csv")
    plan = manifest/(session+".plan.csv")
    trials = manifest/(session+".trials.csv")
    inputs = [meta_file, raw, plan, trials, before, after, attempt/"request.json", attempt/"batch.txt"]
    hashes = {str(path): digest(path) for path in inputs}
    planned, recorded = read_csv(plan), read_csv(trials)
    logged = {r["trial_id"]: r for r in recorded}
    samples = {}
    for row in read_csv(raw):
        samples.setdefault(row["trial_id"], []).append(row)
    ids = {r["trial_id"] for r in planned}
    batch = request["pairs"]
    require(len(logged) == len(recorded) and len(planned) == len(ids) == 2*len(batch)
            and set(logged) <= ids and set(samples) <= ids, "Session differs from the requested batch")
    accepted, rejected = [], {}
    for index, pair in enumerate(batch):
        profiles = {}
        try:
            for leg, direction in enumerate((pair["first"], -pair["first"])):
                tid = str(index*2 + leg + 1)
                start, end = endpoints(p, direction)
                e = planned[index*2 + leg]
                require(e["trial_id"] == tid and e["session_id"] == session and e["phase"] == "AF"
                        and int(e["group"]) == pair["round"] and e["use"] == pair["split"]
                        and int(e["direction"]) == direction and math.isclose(float(e["speed_mm_s"])*1000, pair["speed"])
                        and math.isclose(float(e["approach_mm"])*1000, start)
                        and math.isclose(float(e["target_mm"])*1000, end), f"Trial {tid} differs from frozen plan")
                t = logged.get(tid)
                require(t is not None and t["status"] == "completed" and all(t[k] == v for k, v in e.items())
                        and float(t["period_s"]) == .005, f"Trial {tid} missing, invalid or incomplete")
                rows = samples.get(tid, [])
                require(rows and all(r["valid"] == "1" and all(r[k] == t[k] for k in
                        ("session_id", "trial_id", "phase", "group", "use", "direction", "speed_mm_s"))
                        for r in rows), f"Trial {tid} has invalid samples")
                x, velocity, acceleration, y = longest_plateau(rows, t, model["scan_policy"], meta, model)
                require(x[0] <= grid[0] and x[-1] >= grid[-1],
                        f"Trial {tid} plateau [{x[0]:.1f}, {x[-1]:.1f}] does not cover the frozen grid")
                profiles[str(direction)] = dict(trial=int(tid), lo=float(x[0]), hi=float(x[-1]),
                    v=np.interp(grid, x, velocity).tolist(),
                    y=np.interp(grid, x, y-frozen["alpha"]*acceleration).tolist())
            accepted.append(dict(pair, session=session, field_session=request["field_session"], profiles=profiles))
        except ValueError as error:
            rejected[pair["pair"]] = str(error)
    require(all(digest(path) == hashes[str(path)] for path in inputs), "Inputs changed during replay")
    return dict(session=session, outcome=meta["outcome"], accepted=accepted, rejected=rejected, inputs=hashes)


def fit(development):
    models = {"F0": {}, "F1": {}}
    for direction in ("1", "-1"):
        scans = [row["profiles"][direction] for row in development]
        x = [np.mean(np.abs(p["v"]))/1000 for p in scans]
        y = [np.mean(p["y"]) for p in scans]
        slope, intercept = np.polyfit(x, y, 1)
        models["F0"][direction] = [float(np.mean(y)), 0.]
        models["F1"][direction] = [float(intercept), float(slope)]
    return models


def scores(rows, models, split):
    for name, branches in models.items():
        for direction, (intercept, slope) in branches.items():
            error = np.concatenate([np.array(row["profiles"][direction]["y"])
                        - intercept - slope*np.abs(row["profiles"][direction]["v"])/1000 for row in rows])
            yield dict(split=split, model=name, direction=direction, pairs=len(rows),
                       rmse_raw=np.sqrt(np.mean(error**2)), p95_absolute_raw=np.percentile(np.abs(error), 95),
                       bias_raw=np.mean(error))


def summarize(directory, protocol, accepted, pairs, grid, load, save):
    output = ROOT/"results/formal_a"/protocol
    output.mkdir(parents=True, exist_ok=True)
    profiles, pair_rows, contrast = [], [], {}
    for row in pairs:
        if row["pair"] not in accepted:
            continue
        selected = accepted[row["pair"]]
        p, n = (selected["profiles"][d] for d in ("1", "-1"))
        pos, neg = np.array(p["y"]), np.array(n["y"])
        odd, even = (pos-neg)/2, (pos+neg)/2
        contrast[row["round"], row["speed"]] = float(odd.mean()), selected["field_session"]
        pair_rows.append(dict(row, session=selected["session"], field_session=selected["field_session"],
                              positive_raw=pos.mean(), negative_raw=neg.mean(), half_diff_raw=odd.mean(),
                              half_sum_raw=even.mean(), positive_lo=p["lo"], positive_hi=p["hi"],
                              negative_lo=n["lo"], negative_hi=n["hi"]))
        for i, x in enumerate(grid):
            profiles.append(dict(pair=row["pair"], round=row["round"], speed=row["speed"], split=row["split"],
                                 position_pulse=x, positive_velocity_pulse_s=p["v"][i], negative_velocity_pulse_s=n["v"][i],
                                 positive_raw=pos[i], negative_raw=neg[i], half_diff_raw=odd[i], half_sum_raw=even[i]))
    differences = []
    for r in range(1, 6):
        if (r, 10000) in contrast and (r, 20000) in contrast:
            low, low_session = contrast[r, 10000]
            high, high_session = contrast[r, 20000]
            differences.append(dict(round=r, half_diff_10000_raw=low, half_diff_20000_raw=high,
                                    delta_raw=high-low, field_session_10000=low_session,
                                    field_session_20000=high_session,
                                    same_field_session=low_session.casefold() == high_session.casefold()))
    for name, rows in (("pairs", pair_rows), ("profiles", profiles), ("middle_speed_by_round", differences)):
        if rows:
            write_csv(output/(name+".csv"), rows)
    if any(row["round"] <= 4 and row["pair"] not in accepted for row in pairs):
        return
    model_path = directory/"models_frozen.json"
    splits = {s: [accepted[row["pair"]] for row in pairs if row["split"] == s and row["pair"] in accepted]
              for s in ("development", "speed_validation", "independent_session")}
    if not model_path.is_file():
        save(model_path, dict(protocol=protocol, models=fit(splits["development"]),
                              velocity_unit="1000 pulse/s (absolute)", parameter_order=["intercept", "slope"],
                              development_pairs=[p["pair"] for p in splits["development"]],
                              protocol_sha256=digest(directory/"protocol.json")))
    frozen = load(model_path)
    require(frozen["protocol_sha256"] == digest(directory/"protocol.json"), "Frozen model protocol differs")
    metrics = list(scores(splits["speed_validation"], frozen["models"], "speed_validation"))
    if len(splits["independent_session"]) == 17:
        metrics.extend(scores(splits["independent_session"], frozen["models"], "independent_session"))
    write_csv(output/"model_metrics.csv", metrics)
