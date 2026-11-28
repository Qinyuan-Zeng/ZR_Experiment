"""Evaluate the frozen ZR current-to-force map on supervised open-loop recordings."""

import argparse
import csv
import hashlib
import json
import math
import statistics as stats
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MODEL = ROOT / "models/current_force_preliminary_v1.json"
RAW_SUFFIX = ".open_loop_observe.csv"
OK_FIELDS = ("position_rc", "velocity_rc", "status_rc", "mode_rc", "command_rc",
             "current_rc", "adc_rc", "switch_rc")
FAULT_MASK = (1 << 1) | (1 << 5) | (1 << 6)
ENABLE_MASK, MOVING_MASK = 1 << 9, 1 << 10
MAX_POLL_GAP_US = 50_000
MIN_HOLD_US = 2_800_000
TAIL_US = 1_000_000
POSITION_TOL_PULSE = 10
MAX_HOLD_SPAN_PULSE = 7.5
FORCE_MARGIN_G = 0.5  # End-point tolerance for the measured force span, reported below.


def read_manifest(path):
    return dict(line.split("=", 1) for line in path.read_text(encoding="utf-8-sig").splitlines() if "=" in line)


def frozen_model():
    model = json.loads(MODEL.read_text(encoding="utf-8-sig"))
    source = ROOT / model["source_result"]
    if hashlib.sha256(source.read_bytes()).hexdigest() != model["source_result_sha256"]:
        raise ValueError("Frozen calibration source hash mismatch")
    fit = json.loads(source.read_text(encoding="utf-8-sig"))
    expected_hashes = {r["session"]: r["manifest_sha256"] for r in fit["quality_and_holds_by_trial"]}
    scales = set()
    for name in model["source_sessions"]:
        path = ROOT / "manifest" / f"{name}.txt"
        if hashlib.sha256(path.read_bytes()).hexdigest() != expected_hashes[name]:
            raise ValueError(f"Source manifest hash mismatch: {name}")
        source_manifest = read_manifest(path)
        scales.add((float(source_manifest["zero_v"]), float(source_manifest["scale_display_g_per_v"])))
    if len(scales) != 1:
        raise ValueError("Source sessions do not share an ADC conversion")
    zero, gain = scales.pop()
    return model, zero, gain


def get_rows(path):
    if not path.name.endswith(RAW_SUFFIX):
        raise ValueError(f"Expected {RAW_SUFFIX}: {path}")
    session = path.name.removesuffix(RAW_SUFFIX)
    meta = read_manifest(ROOT / "manifest" / f"{session}.txt")
    if (meta.get("session") != session or meta.get("controller_operations") != "read_only"
            or meta.get("GTN_Close.rc") != "0" or int(meta.get("invalid_samples", "-1")) != 0):
        raise ValueError(f"Incomplete observer record: {session}")
    if meta.get("outcome") not in ("recorded_open_loop_transfer_trace", "recorded_with_position_drift",
                                  "observed_at_reference"):
        raise ValueError(f"Observer record not complete: {session}")
    with path.open(newline="", encoding="utf-8-sig") as stream:
        rows = []
        for row in csv.DictReader(stream):
            rows.append((
                int(row["host_end_us"]), float(row["position_pulse"]), int(row["loop_mode"]),
                float(row["force_command_register_sdk"]), int(row["current_feedback_raw"]),
                float(row["ai1_voltage_v"]), int(row["axis_status"]), int(row["switch_enable"]),
                all(row[field] == "0" for field in OK_FIELDS),
            ))
    if len(rows) != int(meta.get("samples", "-1")) or not rows:
        raise ValueError(f"CSV row count does not match the manifest: {session}")
    if any(b[0] <= a[0] for a, b in zip(rows, rows[1:])):
        raise ValueError(f"Host time must increase: {session}")
    return session, rows


def eligible(row, mode):
    _, position, actual_mode, command, _, voltage, status, switch, reads_ok = row
    return (reads_ok and actual_mode == mode and switch == 0 and status & ENABLE_MASK
            and not status & (FAULT_MASK | MOVING_MASK)
            and all(math.isfinite(v) for v in (position, command, voltage))
            and abs(voltage) < 9.99)


def stable_holds(rows, mode, target):
    candidate = []
    for row in rows:
        if not eligible(row, mode) or abs(row[1] - target) > POSITION_TOL_PULSE:
            if candidate:
                yield candidate
                candidate = []
            continue
        if candidate:
            gap = row[0] - candidate[-1][0]
            if gap > MAX_POLL_GAP_US or row[3] != candidate[0][3]:
                yield candidate
                candidate = []
        candidate.append(row)
    if candidate:
        yield candidate


def switch_summary(rows, open_mode):
    for i, first in enumerate(rows):
        if first[2] != open_mode or not first[-1] or not i or rows[i - 1][2] != 2:
            continue
        before = rows[max(0, i - 200):i]
        hold = [r for r in before if first[0] - r[0] <= TAIL_US and eligible(r, 2)
                and abs(r[1] - 15670) <= POSITION_TOL_PULSE]
        if len(hold) < 10 or hold[-1][0] - hold[0][0] < 500_000:
            return None
        ref = stats.median(r[4] for r in hold)
        return dict(holding_current_raw=ref, first_open_current_raw=first[4],
                    measured_current_step_raw=first[4] - ref,
                    first_open_position_pulse=first[1],
                    last_closed_position_pulse=hold[-1][1],
                    switch_observation_gap_ms=(first[0] - hold[-1][0]) / 1000)
    return None


def point_results(rows, model, zero, gain, open_mode):
    output = []
    conditions = model["conditions"]
    min_x, max_x = conditions["fitted_current_residual_span_raw"]
    min_f, max_f = conditions["measured_force_span_display_g"]
    for node in model["unloaded_reference_by_position_pulse"]:
        target, reference = node["position_pulse"], node["unloaded_current_raw"]
        for hold in stable_holds(rows, open_mode, target):
            duration = hold[-1][0] - hold[0][0]
            if duration < MIN_HOLD_US:
                continue
            positions = [r[1] for r in hold]
            position_span = max(positions) - min(positions)
            if position_span > MAX_HOLD_SPAN_PULSE:
                continue
            tail = [r for r in hold if hold[-1][0] - r[0] < TAIL_US]
            if len(tail) < 10:
                continue
            current = stats.median(r[4] for r in tail)
            voltage = stats.median(r[5] for r in tail)
            residual = current - reference
            measured = (voltage - zero) * gain
            predicted = model["slope_display_g_per_current_raw"] * residual + model["intercept_display_g"]
            output.append(dict(target_pulse=target, duration_s=duration / 1e6, tail_samples=len(tail),
                               position_span_pulse=position_span, force_command_readback_sdk=hold[0][3],
                               current_feedback_raw=current, closed_loop_unloaded_reference_raw=reference,
                               residual_current_raw=residual, ai1_v=voltage, measured_display_g=measured,
                               predicted_display_g=predicted, error_g=predicted - measured,
                               current_within_frozen_span=min_x <= residual <= max_x,
                               force_within_test_span=min_f - FORCE_MARGIN_G <= measured <= max_f + FORCE_MARGIN_G))
    return output


def evaluate(paths, open_mode):
    model, zero, gain = frozen_model()
    trials, errors = [], []
    covered = set()
    for path in paths:
        session, rows = get_rows(path)
        points = point_results(rows, model, zero, gain, open_mode)
        in_range = [p for p in points if p["current_within_frozen_span"] and p["force_within_test_span"]]
        means = {}
        for target in sorted({p["target_pulse"] for p in in_range}):
            means[target] = stats.mean(p["error_g"] for p in in_range if p["target_pulse"] == target)
        measured_errors = list(means.values())
        if measured_errors:
            errors.append(measured_errors)
        covered.update(means)
        trials.append(dict(session=session, source_csv=str(path), switch_observation=switch_summary(rows, open_mode),
                           holds=points, in_range_point_mean_errors_g=means))
    required = set(model["conditions"]["valid_nominal_position_pulse"])
    return dict(calibration_id=model["calibration_id"], model_sha256=hashlib.sha256(MODEL.read_bytes()).hexdigest(),
                strategy="unchanged slope, intercept and closed-loop unloaded references",
                verified_open_loop_mode=open_mode, adc_zero_v=zero, adc_gain_display_g_per_v=gain,
                force_endpoint_margin_g=FORCE_MARGIN_G, positions_covered_pulse=sorted(covered),
                positions_missing_pulse=sorted(required - covered),
                sessions_with_in_range_data=len(errors), trials=trials,
                closed_loop_leave_pair_out_rmse_reference_g=model["evaluation"]["leave_trial_pair_out_rmse_display_g"],
                open_loop_rmse_g=math.sqrt(stats.mean(stats.mean(e * e for e in group) for group in errors))
                    if errors else None,
                open_loop_bias_g=stats.mean(stats.mean(group) for group in errors) if errors else None,
                open_loop_max_abs_error_g=max(abs(e) for group in errors for e in group) if errors else None)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--open-mode", required=True, type=int,
                        help="Numeric open-loop mode independently verified on this ZR")
    parser.add_argument("--output", type=Path, help="Create a new JSON report")
    parser.add_argument("csv_paths", nargs="+", type=Path,
                        help="Independent loaded observer captures, in the same fixture")
    args = parser.parse_args()
    paths = [p.resolve() for p in args.csv_paths]
    if not 0 <= args.open_mode <= 32767 or args.open_mode == 2 or len(set(paths)) != len(paths):
        parser.error("Specify a verified non-position mode and unique CSV files")
    try:
        result = evaluate(paths, args.open_mode)
        text = json.dumps(result, ensure_ascii=False, indent=2, allow_nan=False) + "\n"
        if args.output:
            with args.output.open("x", encoding="utf-8") as stream:
                stream.write(text)
        print(text, end="")
    except (ValueError, OSError, KeyError, TypeError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
