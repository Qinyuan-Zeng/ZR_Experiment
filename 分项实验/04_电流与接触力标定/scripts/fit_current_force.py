"""Fit a quasi-static spring map from paired loaded/unloaded A0 hold captures."""

import argparse
import csv
import json
import math
import statistics as st
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TARGETS = {1: 16670, 2: 17670, 3: 18670}


def read_trial(session):
    if not session.startswith("Z-FA0-MAP_") or not all(c.isalnum() or c in "-_" for c in session):
        raise ValueError(f"Invalid mapping session ID: {session}")
    manifest = dict(
        line.split("=", 1) for line in
        (ROOT / "manifest" / f"{session}.txt").read_text(encoding="utf-8-sig").splitlines()
        if "=" in line
    )
    if (manifest.get("outcome") != "mapping_complete_pending_independent_review"
            or manifest.get("protocol") != "Z-FA0-SPRING-01-MAP-200"):
        raise ValueError(f"{session}: incomplete or different protocol")
    condition = manifest.get("condition")
    if condition not in ("loaded", "unloaded"):
        raise ValueError(f"{session}: unrecognized specimen condition")
    with (ROOT / "data" / "raw" / f"{session}.current_force.csv").open(newline="") as stream:
        rows = list(csv.DictReader(stream))
    trials = {}
    for point, target in TARGETS.items():
        hold = [
            row for row in rows if row["phase"] == "hold"
            and int(row["point"]) == point and int(row["target_pulse"]) == target
        ]
        times = [int(row["adc_host_us"]) for row in hold]
        if not times or times[-1] - times[0] < 2_800_000:
            raise ValueError(f"{session}: short hold at {target} pulse")
        tail = [row for row, stamp in zip(hold, times) if stamp > times[-1] - 1_000_000]
        if len(tail) < 10:
            raise ValueError(f"{session}: insufficient tail samples at {target} pulse")
        currents, forces, positions = [], [], []
        for row in tail:
            if row["adc_rc"] != "0" or row["current_rc"] != "0":
                raise ValueError(f"{session}: invalid tail sample")
            currents.append(float(row["current_raw"]))
            forces.append(float(row["force_display_g"]))
            positions.append(float(row["position_pulse"]))
        if not all(math.isfinite(x) for group in (currents, forces, positions) for x in group):
            raise ValueError(f"{session}: nonfinite tail data")
        if any(abs(x - target) > 10 for x in positions):
            raise ValueError(f"{session}: measured hold outside target tolerance")
        trials[point] = (st.median(currents), st.median(forces))
    return condition, trials


def point_pairs(loaded, unloaded):
    points = []
    for index, target in TARGETS.items():
        with_load = [trial[index] for trial in loaded]
        free = [trial[index] for trial in unloaded]
        loaded_current = st.mean(sample[0] for sample in with_load)
        unloaded_current = st.mean(sample[0] for sample in free)
        force_g = st.mean(sample[1] for sample in with_load) - st.mean(sample[1] for sample in free)
        points.append({
            "position_pulse": target,
            "loaded_current_raw": loaded_current,
            "unloaded_current_raw": unloaded_current,
            "delta_current_raw": loaded_current - unloaded_current,
            "force_display_g_zero_corrected": force_g,
        })
    return points


def fit(points):
    x = [row["delta_current_raw"] for row in points]
    y = [row["force_display_g_zero_corrected"] for row in points]
    if len(set(x)) < 2 or not (0 < y[0] < y[1] < y[2]):
        raise ValueError("Holds do not resolve an increasing load and distinct current residuals")
    if not (x[0] < x[1] < x[2] or x[0] > x[1] > x[2]):
        raise ValueError("Current residual is not monotonic across the three loads")
    k, b = st.linear_regression(x, y)
    return k, b


def analyze(session_ids):
    loaded, unloaded = [], []
    for session in session_ids:
        condition, trial = read_trial(session)
        (loaded if condition == "loaded" else unloaded).append(trial)
    if not loaded or not unloaded:
        raise ValueError("Need one complete loaded run and one matched unloaded run")
    points = point_pairs(loaded, unloaded)
    k, b = fit(points)
    residuals = [k * row["delta_current_raw"] + b - row["force_display_g_zero_corrected"]
                 for row in points]
    held_out_rmse = None
    if len(loaded) >= 2 and len(unloaded) >= 2:
        errors = []
        for i in range(min(len(loaded), len(unloaded))):
            train_loaded = loaded[:i] + loaded[i + 1:]
            train_unloaded = unloaded[:i] + unloaded[i + 1:]
            test_slope, test_intercept = fit(point_pairs(train_loaded, train_unloaded))
            errors.extend(test_slope * row["delta_current_raw"] + test_intercept
                          - row["force_display_g_zero_corrected"]
                          for row in point_pairs([loaded[i]], [unloaded[i]]))
        held_out_rmse = math.sqrt(st.mean(value * value for value in errors))
    return {
        "basis": "quasi-static holds and instrument-equivalent force",
        "source_sessions": session_ids,
        "loaded_trials": len(loaded),
        "unloaded_trials": len(unloaded),
        "points": points,
        "slope_display_g_per_current_raw": k,
        "intercept_display_g": b,
        "slope_inferred_N_per_current_raw": k * 0.00980665,
        "in_sample_rmse_display_g": math.sqrt(st.mean(e * e for e in residuals)),
        "leave_pair_out_rmse_display_g": held_out_rmse,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sessions", nargs="+", help="Independently reviewed Z-FA0-MAP session IDs")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if len(set(args.sessions)) != len(args.sessions):
        parser.error("Duplicate session IDs")
    try:
        result = json.dumps(analyze(args.sessions), ensure_ascii=False, indent=2) + "\n"
    except (ValueError, OSError, KeyError) as error:
        parser.exit(1, f"{error}\n")
    if args.output:
        with args.output.open("x", encoding="utf-8") as stream:
            stream.write(result)
    print(result, end="")


if __name__ == "__main__":
    main()
