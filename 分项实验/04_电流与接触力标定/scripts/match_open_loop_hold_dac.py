"""Estimate a ZR hold DAC from measured open-loop DAC and current plateaus."""

import argparse
import csv
import json
import math
import statistics as stats
from collections import defaultdict
from pathlib import Path

from validate_open_loop_transfer import ROOT, MAX_HOLD_SPAN_PULSE, get_rows, read_manifest, stable_holds

START_PULSE = 15670
MIN_HOLD_US = 800_000
TAIL_US = 500_000


def collect(paths, mode):
    data = defaultdict(list)
    sessions = []
    for path in paths:
        session, rows = get_rows(path)
        sessions.append(session)
        for hold in stable_holds(rows, mode, START_PULSE):
            positions = [r[1] for r in hold]
            if (hold[-1][0] - hold[0][0] < MIN_HOLD_US
                    or max(positions) - min(positions) > MAX_HOLD_SPAN_PULSE):
                continue
            dac = hold[0][3]
            if not dac.is_integer() or not -32768 <= dac <= 32767:
                continue
            tail = [r[4] for r in hold if hold[-1][0] - r[0] < TAIL_US]
            if len(tail) >= 10:
                data[int(dac)].append(stats.median(tail))
    return sessions, [{
        "dac": dac, "current_raw": stats.median(values),
        "hold_medians_raw": values,
    } for dac, values in sorted(data.items())]


POINTS_SUFFIX = ".dac_current_points.csv"


def collect_sweep_points(paths):
    data = defaultdict(list)
    sessions = []
    for path in paths:
        if not path.name.endswith(POINTS_SUFFIX):
            raise ValueError(f"Expected {POINTS_SUFFIX}: {path}")
        session = path.name.removesuffix(POINTS_SUFFIX)
        meta = read_manifest(ROOT / "manifest" / f"{session}.txt")
        if (meta.get("session") != session or meta.get("protocol") not in ("Z-FA0-OPEN-DAC-CURRENT-01", "Z-FA0-OPEN-ZERO-DAC-01")
                or meta.get("outcome") != "complete_start_dac_confirmed" or meta.get("GTN_Close.rc") != "0"):
            raise ValueError(f"Incomplete DAC experiment: {session}")
        plan = [int(value) for value in meta["plan_dac"].split()]
        with path.open(newline="", encoding="utf-8-sig") as stream:
            rows = list(csv.DictReader(stream))
        if not plan or len(plan) != len(rows) or plan != plan[::-1]:
            raise ValueError(f"Plateaus do not match a complete round trip: {session}")
        for step, (dac, row) in enumerate(zip(plan, rows)):
            if (int(row["step"]) != step or int(row["dac"]) != dac
                    or int(row["tail_samples"]) < 10):
                raise ValueError(f"Plateau does not match the command plan: {session}, step {step}")
            low, current, high = [float(row[key]) for key in
                                  ("current_min_raw", "current_median_raw", "current_max_raw")]
            if not (math.isfinite(current) and -32768 <= low <= current <= high <= 32767):
                raise ValueError(f"Invalid current summary: {session}, step {step}")
            data[dac].append(current)
        sessions.append(session)
    return sessions, [dict(dac=dac, current_raw=stats.median(values), hold_medians_raw=values)
                      for dac, values in sorted(data.items())]


def describe(points):
    if len(points) < 2:
        return None
    dac = [p["dac"] for p in points]
    current = [p["current_raw"] for p in points]
    slope, intercept = stats.linear_regression(dac, current)
    residuals = [slope * x + intercept - y for x, y in zip(dac, current)]
    return dict(slope_raw_per_dac=slope, intercept_raw=intercept,
                rmse_raw=math.sqrt(stats.mean(error * error for error in residuals)),
                measured_dac_span=[min(dac), max(dac)],
                measured_current_span_raw=[min(current), max(current)],
                max_repeat_current_span_raw=max(max(p["hold_medians_raw"]) - min(p["hold_medians_raw"])
                                                for p in points))


def match(points, target):
    if len(points) < 2:
        return None, "need_two_measured_dac_levels"
    currents = [point["current_raw"] for point in points]
    changes = [b - a for a, b in zip(currents, currents[1:])]
    if not (all(change > 0 for change in changes) or all(change < 0 for change in changes)):
        return None, "current_not_strictly_monotonic"
    for point in points:
        if point["current_raw"] == target:
            return point["dac"], "measured_match"
    for a, b in zip(points, points[1:]):
        low, high = sorted((a["current_raw"], b["current_raw"]))
        if low < target < high:
            ratio = (target - a["current_raw"]) / (b["current_raw"] - a["current_raw"])
            candidate = round(a["dac"] + ratio * (b["dac"] - a["dac"]))
            return candidate, "interpolated_needs_confirmation"
    return None, "target_outside_measured_current_range"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--open-mode", type=int, help="Verified GVN open-loop mode for observer CSVs")
    parser.add_argument("--points", action="store_true", help="Use completed DAC-experiment plateau summaries")
    parser.add_argument("--hold-raw", type=float, required=True, help="Measured closed-loop holding current")
    parser.add_argument("csv_paths", type=Path, nargs="+")
    args = parser.parse_args()
    if not math.isfinite(args.hold_raw):
        parser.error("Specify a finite holding current")
    if not args.points and (args.open_mode is None or args.open_mode == 2
                            or not 0 <= args.open_mode <= 32767):
        parser.error("Specify the verified open-loop mode for observer CSVs")
    paths = [p.resolve() for p in args.csv_paths]
    if len(set(paths)) != len(paths):
        parser.error("Duplicate capture paths")
    try:
        sessions, points = collect_sweep_points(paths) if args.points else collect(paths, args.open_mode)
        dac, status = match(points, args.hold_raw)
    except (ValueError, OSError, KeyError, TypeError) as error:
        parser.exit(1, f"{error}\n")
    print(json.dumps(dict(target_current_raw=args.hold_raw, session_ids=sessions,
                          points=points, linear_fit=describe(points), hold_dac_candidate=dac, status=status), indent=2))
    return 0 if dac is not None else 2


if __name__ == "__main__":
    raise SystemExit(main())
