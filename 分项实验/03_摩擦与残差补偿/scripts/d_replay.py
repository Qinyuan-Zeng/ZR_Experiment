"""D HOLD window audit. Historical cohorts and synchronized arrival windows stay separate."""
import argparse
from pathlib import Path

import numpy as np
import pandas as pd

import d_run as d
from a0_replay import MODEL, digest, require
from a_formal import identifier, load, locked, save
from b1_run import fields

ROOT = d.ROOT
REPO = ROOT.parents[1]
B1_ID = "Z-B1-20261127-02"
C_ID = "Z-C-HISTORY-01"
WINDOWS = [(0, .2), (.2, 1), (1, 3), (3, 4), (4, 6)]
IP = load(MODEL)


def tick_index(meta, event, first_tick):
    return (int(meta[event + "_tick"]) - int(first_tick)) % (1 << 32)


def windows(t, x, current, last, gap, record):
    require(len(t) >= 2 and np.diff(t).min() > 0 and np.isfinite(t).all(), "Invalid HOLD clock")
    low, high = IP["range_pulse"]
    target = record["target_pulse"]
    for left, right in WINDOWS:
        # Keep incomplete and unstable windows visible in the coverage table.
        select = (t >= left - gap * 1e-4) & (t < right - gap * 1e-4)
        tu, xu, iu = t[select], x[select], current[select]
        ok = (last >= right - 1e-8 and len(tu) >= 2 and
              tu[0] - left <= gap + 1e-8 and right - tu[-1] <= gap + 1e-8 and
              np.diff(tu).max() <= gap + 1e-8 and np.isfinite(xu).all() and np.isfinite(iu).all() and
              np.all((xu >= low) & (xu <= high)) and np.abs(xu - target).max() <= 10 + 1e-8)
        out = dict(record, begin_s=left, end_s=right, samples=len(tu), valid=bool(ok))
        if ok:
            residual = iu - np.interp(xu, IP["node_pulse"], IP["current_raw"])
            out.update(mean_raw=float(residual.mean()), rmse_raw=float(np.sqrt(np.mean(residual**2))),
                       mae_raw=float(np.abs(residual).mean()), p95_abs_raw=float(np.quantile(np.abs(residual), .95)))
        yield out


def watch(inputs, record, start_event, end_events):
    for path, sha in inputs.items():
        require(digest(path) == sha, f"Accepted input changed: {path}")
    raw = next(path for path in inputs if path.name.endswith(".csv") and
               not path.name.endswith((".sdk.csv", ".batches.csv")))
    meta = fields(next(path for path in inputs if path.suffix == ".txt"))
    w = pd.read_csv(raw)
    ticks = w.card_tick_raw.to_numpy(dtype=np.int64)
    unwrapped = (ticks - ticks[0]) % (1 << 32)
    require(int(meta["core_period_ns"]) == d.PERIOD_NS and np.all(np.diff(unwrapped) == 1),
            f"Unexpected Watch clock: {raw}")
    start = tick_index(meta, start_event, ticks[0])
    stop = min(tick_index(meta, event, ticks[0]) for event in end_events)
    require(0 <= start < stop < len(ticks), f"Missing HOLD event window: {raw}")
    sl = slice(start, stop+1)
    t = (unwrapped[sl] - start) * d.PERIOD_NS / 1e9
    gap = d.PERIOD_NS / 1e9
    yield from windows(t, w.encoder_position_raw.to_numpy()[sl], w.current_feedback_raw.to_numpy()[sl],
                       t[-1], gap, dict(record, raw_path=raw.relative_to(REPO).as_posix(),
                                       clock="watch_arrival_confirmation", period_ms=gap * 1000))


def legacy():
    base = MODEL.parents[1]
    sources = [("stage1_B", "Z-B_20261124-170633_02", "position_model_fit"),
               ("stage1_D1", "Z-D1_20261124-201558_01", "position_model_validation"),
               ("stage1_D2", "Z-D2_20261124-202754_01", "position_model_validation")]
    for cohort, session, role in sources:
        raw = base / "data/raw" / (session + ".csv")
        trials = base / "manifest" / (session + ".trials.csv")
        meta = fields(base / "manifest" / (session + ".txt"))
        for key, expected in IP["conditions"].items():
            require(meta.get(key) == expected, f"Historic HOLD configuration mismatch: {session}, {key}")
        sha = digest(raw)
        require(IP["raw_sha256"].get(session, sha) == sha, f"Position model source changed: {session}")
        trial = pd.read_csv(trials).query("status == 'completed'").set_index("trial_id")
        data = pd.read_csv(raw, low_memory=False)
        for tid, w in data.loc[data.stage.eq("hold")].groupby("trial_id", sort=False):
            if tid not in trial.index:
                continue
            q = trial.loc[tid]
            if int(q.direction) not in (-1, 1):
                continue
            t = w.t_s.to_numpy() - w.t_s.iloc[0]
            good = (w.valid.eq(1) & w.enabled.eq(1) & w.alarm.eq(0) & w["mode"].eq(2) &
                    w.limit_pos.eq(0) & w.limit_neg.eq(0) & ((w.status.astype(int) & (1 << 10)) == 0))
            record = dict(cohort=cohort, session=session, run_id=str(tid), hold_phase="arrival",
                          target_pulse=round(q.target_mm*1000), direction=int(q.direction), repeat=q.group,
                          speed_mm_s=q.speed_mm_s, role=role, clock="legacy_first_hold_sample",
                          period_ms=float(np.median(np.diff(t))) * 1000, raw_path=raw.relative_to(REPO).as_posix())
            # Legacy holds are timed from the first recorded HOLD sample. The
            # final sample may precede the planned boundary by one real interval.
            gap = 2.5 * float(np.median(np.diff(t)))
            end = min(float(q.hold_s), t[-1] + gap)
            yield from windows(t, w.x_mm.to_numpy()*1000, w.current_fb_raw.where(good).to_numpy(),
                               end, gap, record)


def b1():
    for file in sorted((ROOT / "B1/manifest" / B1_ID / "attempts").glob("*/acceptance.json")):
        for r in load(file)["accepted"]:
            for side, sign in (("positive", 1), ("negative", -1)):
                detail = r["leg_inputs"][side]
                inputs = {Path(item["path"]): item["sha256"] for item in detail.values()}
                meta = fields(Path(detail["metadata"]["path"]))
                yield from watch(inputs, dict(cohort="B1", session=r["field_session"], run_id=r["slot_id"] + ":" + side,
                                              hold_phase="arrival", target_pulse=int(meta["end_pulse"]), direction=sign,
                                              repeat=r["round"], speed_mm_s=r["speed_mm_s"], role="stage3_development"),
                                 "settled", ("post_hold_complete",))


def c_history():
    for file in sorted((ROOT / "C/manifest" / C_ID / "attempts").glob("*/acceptance.json")):
        for r in load(file)["accepted"]:
            inputs = {Path(path): sha for path, sha in r["inputs"].items()}
            reverse = r["history"] == "reverse"
            record = dict(cohort="C", session=r["field_session"], run_id=r["id"], repeat=r["repeat"],
                          role="stage3_development", history=r["history"])
            yield from watch(inputs, dict(record, hold_phase="center_dwell", target_pulse=16000,
                                         direction=r["sign"] * (-1 if reverse else 1),
                                         speed_mm_s=2 if r["history"]=="direct" else 10),
                             "approach_settled", ("probe_before", "dwell_end"))
            yield from watch(inputs, dict(record, hold_phase="probe_tail", target_pulse=r["end"],
                                         direction=r["sign"], speed_mm_s=r["speed"]),
                             "probe_settled", ("post_hold_end",))


def d_runs(protocol):
    folder = d.D / "manifest" / protocol
    with locked(folder):
        frozen = d.verified(protocol, folder)
        runs = d.replay(folder, frozen, verify_sources=True)
    for r in runs.values():
        inputs = {Path(path): sha for path, sha in r["inputs"].items()}
        yield from watch(inputs, dict(cohort="stage3_D", session=r["field_session"], run_id=r["id"],
                                     hold_phase="arrival", target_pulse=r["target"], direction=r["sign"],
                                     repeat=r["repeat"], speed_mm_s=10, role="stage3_development"),
                         "approach_settled", ("hold_end",))


def outputs(rows, output):
    table = pd.DataFrame(rows)
    require(not table.empty and not table.duplicated(
        ["cohort","session","run_id","hold_phase","begin_s","end_s"]).any(), "Empty or duplicate D windows")
    coverage = table.groupby(["cohort","clock","hold_phase","begin_s","end_s"], as_index=False).agg(
        run_windows=("valid","size"), valid_windows=("valid","sum"))
    good = table.loc[table.valid]
    keys = ["cohort","session","hold_phase","target_pulse","direction","speed_mm_s","begin_s","end_s"]
    summary = good.groupby(keys, as_index=False).agg(
        independent_runs=("run_id","nunique"), bias_raw=("mean_raw","mean"), repeat_sd_raw=("mean_raw","std"),
        mean_run_mae_raw=("mae_raw","mean"), mean_run_p95_abs_raw=("p95_abs_raw","mean"),
        rmse_raw=("rmse_raw",lambda x: float(np.sqrt(np.mean(x**2)))))
    output.mkdir(parents=True, exist_ok=True)
    for name, frame in (("hold_windows",table),("hold_coverage",coverage),("hold_summary",summary)):
        frame.to_csv(output/(name+".csv"),index=False,encoding="utf-8-sig")
    if "stage3_D" in set(good.cohort):
        d_data = good.loc[good.cohort.eq("stage3_D")]
        pairs = d_data.pivot(index=["session","repeat","target_pulse","begin_s","end_s"],
                             columns="direction",values="mean_raw").reindex(columns=[-1,1]).dropna()
        pairs = pairs.rename(columns={-1:"negative_raw",1:"positive_raw"})
        pairs["positive_minus_negative_raw"] = pairs.positive_raw - pairs.negative_raw
        pairs["center_raw"] = (pairs.positive_raw + pairs.negative_raw)/2
        pairs.to_csv(output/"direction_pairs.csv",encoding="utf-8-sig")
    save(output/"analysis.json",dict(position_model=MODEL.relative_to(REPO).as_posix(),
         position_model_sha256=digest(MODEL), estimator="HOLD current raw minus Ip(actual encoder position)",
         windows_s=WINDOWS, final_E_data_used=False, source_record_roles=sorted(set(table.role))))
    print(coverage.to_string(index=False))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--history", action="store_true")
    group.add_argument("--protocol", type=identifier)
    args = parser.parse_args()
    audit = load(ROOT/"B0/audit_provenance.json")
    require(digest(MODEL)==audit["position_model_sha256"], "Frozen Ip differs from B0")
    if args.history:
        rows = [*legacy(), *b1(), *c_history()]
        outputs(rows,d.D/"results/historical")
    else:
        outputs(list(d_runs(args.protocol)),d.D/"results"/args.protocol)


if __name__ == "__main__":
    main()
