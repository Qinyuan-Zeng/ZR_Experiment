"""Stage 3 C start/stop history acquisition and offline quality replay."""
import argparse
import csv
from itertools import product
from pathlib import Path
import subprocess
import uuid

import numpy as np
import pandas as pd

from a0_replay import digest, require
from a_formal import identifier, load, locked, save
from b1_run import clearance, fields, number, read_state

ROOT = Path(__file__).resolve().parents[1]
C = ROOT / "C"
EXE = ROOT / "bin/x64/Release/c_scan/ZRFrictionCurrent.exe"
CENTER = 16000
STROKE = 4000
PERIOD_NS = 50_000
LIMIT_KEYS = ("lo", "hi", "reserve", "follow", "min_stop")
MEAS_CAP = 15
TRANSFER_CAP = 5


def plan():
    family = [
        (history, speed, sign, dwell)
        for speed, sign in product((2, 5, 10), (1, -1))
        for history, dwell in [
            ("direct", 2000),
            *product(("same", "reverse"), (200, 500, 1000)),
        ]
    ]
    pilot = [
        ("direct", 5, 1, 2000), ("same", 2, 1, 200), ("reverse", 10, 1, 1000),
        ("direct", 5, -1, 2000), ("same", 10, -1, 500), ("reverse", 2, -1, 200),
    ]
    rows = []
    for repeat in (1, 2, 3):
        offset = (repeat - 1) * 13
        ordered = family[offset:] + family[:offset]
        if repeat == 1:
            ordered = pilot + [item for item in ordered if item not in pilot]
        for history, speed, sign, dwell in ordered:
            end = CENTER + sign * STROKE
            start = end if history == "reverse" else CENTER - sign * STROKE
            direction = "P" if sign == 1 else "N"
            rows.append(dict(
                id=f"R{repeat}-{history}-{direction}-V{speed}-T{dwell}", repeat=repeat,
                history=history, sign=sign, speed=speed, dwell=dwell, start=start, end=end,
                pilot=repeat == 1 and (history, speed, sign, dwell) in pilot,
            ))
    return rows


def show(rows):
    print(f"{len(rows)} C trials | 16000 pulse restart point | Watch 50 us")
    for row in rows:
        print(f'{row["id"]:26} {row["start"]} -> {CENTER} -> {row["end"]} pulse'
              f'  dwell {row["dwell"]} ms  probe {row["speed"]} mm/s')


def fingerprints():
    inc = ROOT.parent / "02_等效惯性电流辨识/include"
    paths = [Path(__file__), EXE, EXE.with_name("gts.dll"), ROOT / "src/c_scan.cpp",
             ROOT / "scripts/b1_run.py", ROOT / "scripts/a_formal.py", ROOT / "scripts/a0_replay.py",
             *(inc / name for name in ("matched_batch.hpp", "matched_motion.hpp", "watch_capture.hpp"))]
    return {p.relative_to(ROOT.parent).as_posix(): digest(p) for p in paths}


def freeze(protocol, folder):
    require(not folder.exists(), "C protocol ID already exists")
    l = dict(zip(LIMIT_KEYS, (
        number("Protection lower pulse", 0), number("Protection upper pulse", 37000),
        number("Additional stopping reserve pulse", 4000), number("Following error limit pulse", 500),
        number("Minimum smooth stop pulse/ms^2", .99),
    )))
    require(0 <= l["lo"] < 12000 and 20000 < l["hi"] <= 37000 and l["reserve"] >= 1
            and 1 <= l["follow"] <= 500 and l["min_stop"] > 0, "Unsupported C protection values")
    clearance(l, 12000, 20000, MEAS_CAP, l["min_stop"])
    rows = plan()
    hashes = fingerprints()
    show([row for row in rows if row["pilot"]])
    require(input(f"Type {protocol} to freeze C: ").strip() == protocol, "C freeze cancelled")
    folder.mkdir(parents=True)
    table = folder / "plan.csv"
    with table.open("w", encoding="utf-8-sig", newline="") as file:
        writer = csv.DictWriter(file, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    manifest = folder / "protocol.json"
    save(manifest, dict(id=protocol, limits=l, trials=rows, hashes=hashes, plan_sha256=digest(table),
                        core_period_ns=PERIOD_NS, measurement_cap=MEAS_CAP, transfer_cap=TRANSFER_CAP))
    (folder / "protocol.sha256").write_text(digest(manifest) + "\n", encoding="ascii")
    print(f"{protocol} frozen offline | 126 trials including 6 pilot trials")


def verified(protocol, folder):
    path = folder / "protocol.json"
    require(digest(path) == (folder / "protocol.sha256").read_text().strip(), "C protocol hash mismatch")
    p = load(path)
    require(p["id"] == protocol and p["trials"] == plan() and p["hashes"] == fingerprints() and
            p["plan_sha256"] == digest(folder / "plan.csv"), "C frozen sources or point table changed")
    return p


def check_trial(raw_dir, trial, limits):
    paths = {ext: raw_dir / (trial["id"] + ext) for ext in (".txt", ".csv", ".sdk.csv", ".batches.csv")}
    hashes = {ext: digest(path) for ext, path in paths.items()}
    m = fields(paths[".txt"])
    require(m["schema"] == "c_history_v1" and m["slot_id"] == trial["id"] and
            m["history"] == trial["history"] and int(m["direction"]) == trial["sign"] and
            int(m["probe_speed_mm_s"]) == trial["speed"] and int(m["dwell_ms"]) == trial["dwell"] and
            int(m["core_period_ns"]) == PERIOD_NS and m["outcome"] == "complete" and
            m["watch_aligned_contiguous"] == "1", "Incomplete C capture or mismatched configuration")
    raw = pd.read_csv(paths[".csv"])
    require(list(raw.columns) == ["frame", "card_tick_raw", "encoder_position_raw", "current_feedback_raw"]
            and len(raw) > 8000 and np.isfinite(raw.to_numpy(float)).all(), "Invalid C Watch shape")
    ticks = raw.card_tick_raw.to_numpy(dtype=np.int64)
    offsets = (ticks - ticks[0]) % (1 << 32)
    require(np.all(np.diff(offsets) == 1) and np.array_equal(raw.frame, np.arange(len(raw))),
            "Incomplete C Watch ticks")
    events = ("setup_hold_begin", "setup_hold_end", "approach_before", "approach_after",
              "approach_settled", "dwell_begin", "dwell_end", "probe_before", "probe_after",
              "probe_settled", "post_hold_begin", "post_hold_end")
    e = {name: (int(m[name + "_tick"]) - int(ticks[0])) % (1 << 32) for name in events}
    require(list(e.values()) == sorted(e.values()) and e["post_hold_end"] <= offsets[-1],
            "C event order or coverage error")
    x = raw.encoder_position_raw.to_numpy()
    current = raw.current_feedback_raw.to_numpy()
    require(x.min() >= max(limits["lo"], min(trial["start"], trial["end"], CENTER) - 10) and
            x.max() <= min(limits["hi"], max(trial["start"], trial["end"], CENTER) + 10),
            "C Watch position outside the scheduled path")
    tick_ms = PERIOD_NS / 1e6
    for stage, target, minimum in (("setup_hold", trial["start"], 250),
                                    ("dwell", CENTER, trial["dwell"]),
                                    ("post_hold", trial["end"], 500)):
        begin, end = e[stage + "_begin"], e[stage + "_end"]
        require((end - begin + 1) * tick_ms >= minimum and
                np.abs(x[begin:end + 1] - target).max() <= 10 and
                np.ptp(x[begin:end + 1]) <= 10, f"C {stage} failed duration or position stability")
    dwell = (e["probe_before"] - e["approach_settled"]) * tick_ms
    require(trial["dwell"] - tick_ms <= dwell <= trial["dwell"] + 150,
            "C arrival-to-restart dwell differs from the planned bin")
    lag = 100  # 5 ms Watch-derived velocity reach and protection check.
    for stage, start, end, speed in (
        ("approach", trial["start"], CENTER, 2 if trial["history"] == "direct" else 10),
        ("probe", CENTER, trial["end"], trial["speed"]),
    ):
        begin, finish = e[stage + "_after"], e[stage + "_settled"]
        segment = x[begin:finish + 1]
        require(len(segment) > lag, f"C {stage} motion segment too short")
        velocity = np.sign(end - start) * (segment[lag:] - segment[:-lag]) / 5
        require(abs(segment[0] - start) <= 10 and abs(segment[-1] - end) <= 10 and
                np.abs(velocity).max() <= MEAS_CAP and np.percentile(velocity, 95) >= .9 * speed,
                f"C {stage} endpoint or actual speed failed")
    host = pd.read_csv(paths[".sdk.csv"])
    codes = [key for key in host if key.startswith("rc_")]
    status = host.status.to_numpy(dtype=np.int64)
    require(len(codes) == 9 and host[codes].eq(0).all(axis=None) and
            {"setup_hold", "approach", "dwell", "probe", "post_hold"} <= set(host.phase) and
            host.host_begin_us.diff().dropna().gt(0).all() and
            np.all((status & ((1 << 1) | (1 << 5) | (1 << 6))) == 0) and np.all(status & (1 << 9)) and
            np.abs(host.following_error_pulse).max() <= limits["follow"],
            "C invalid host trace or unsafe state recorded")
    require(all(digest(path) == hashes[ext] for ext, path in paths.items()), "C sources changed during replay")
    return dict(frames=len(raw), watch_period_us=PERIOD_NS / 1000,
                arrival_to_restart_ms=dwell, settled_dwell_ms=(e["dwell_end"] - e["dwell_begin"]) * tick_ms,
                probe_bracket_ms=(e["probe_after"] - e["probe_before"]) * tick_ms,
                dwell_current_raw=float(np.mean(current[e["dwell_begin"]:e["dwell_end"]])),
                probe_start_current_abs_peak_raw=float(np.abs(
                    current[e["probe_before"]:e["probe_before"] + 2000]).max()),
                inputs={str(path): hashes[ext] for ext, path in paths.items()})


def replay(folder, protocol, verify_sources=False):
    trials = {trial["id"]: trial for trial in protocol["trials"]}
    known = {}
    for attempt in sorted((folder / "attempts").glob("*")):
        if not all((attempt / name).is_file() for name in ("pre.txt", "post.txt", "request.json", "request.txt", "exit.json", "native.txt")):
            continue
        result = attempt / "acceptance.json"
        if result.exists():
            evidence = load(result)
            if verify_sources:
                require(all(digest(attempt / name) == sha for name, sha in evidence["batch"].items()),
                        f"C batch evidence changed: {attempt.name}")
        else:
            l = protocol["limits"]
            try:
                read_state(attempt / "pre.txt", l)
                read_state(attempt / "post.txt", l)
                require(fields(attempt / "native.txt")["core_period_ns"] == str(PERIOD_NS), "C clock mismatch")
                request = load(attempt / "request.json")
                require(all(trials[t["id"]] == t for t in request["trials"]), "C executed point table differs")
            except (OSError, ValueError, KeyError) as error:
                print(f"{attempt.name}: {error}")
                continue
            evidence = dict(accepted=[], rejected={}, batch={
                name: digest(attempt / name) for name in ("pre.txt", "post.txt", "request.json", "request.txt", "exit.json", "native.txt")})
            for trial in request["trials"]:
                if trial["id"] in known:
                    continue
                try:
                    data = check_trial(C / "data/raw" / protocol["id"] / attempt.name, trial, l)
                    evidence["accepted"].append(dict(trial, **data, field_session=request["field_session"],
                                                      attempt=attempt.name))
                except (OSError, ValueError, KeyError) as error:
                    evidence["rejected"][trial["id"]] = str(error)
                    print(f'{trial["id"]}: {error}')
            save(result, evidence)
        for row in evidence["accepted"]:
            require(row["id"] not in known, "Duplicate C accepted trial")
            if verify_sources:
                require(all(digest(Path(name)) == sha for name, sha in row["inputs"].items()), "C raw evidence changed")
            known[row["id"]] = row
    print(f"{len(known)}/{len(trials)} C trials accepted")
    if known:
        out = C / "results" / protocol["id"]
        out.mkdir(parents=True, exist_ok=True)
        temp = out / "accepted_trials.tmp"
        pd.DataFrame([{k: v for k, v in known[t].items() if k != "inputs"}
                      for t in trials if t in known]).to_csv(temp, index=False, encoding="utf-8-sig")
        temp.replace(out / "accepted_trials.csv")
    return known


def check_state(path, limits):
    r = subprocess.run([str(EXE), "--state"], capture_output=True, text=True, encoding="ascii", errors="replace")
    path.write_text(r.stdout + r.stderr, encoding="utf-8")
    require(r.returncode == 0, f"C independent state check failed: {path}")
    return read_state(path, limits)


def batch(folder, protocol, pilot):
    known = replay(folder, protocol)
    queue = [r for r in protocol["trials"] if r["id"] not in known and (r["pilot"] or not pilot)]
    if not queue:
        return 0
    first = queue[0]
    rows = [r for r in queue if (r["repeat"], r["pilot"]) == (first["repeat"], first["pilot"])][:3]
    show(rows)
    session = identifier(input("Actual field session ID: ").strip())
    require(input("Confirm current field authorization and safeguards. Type READY: ").strip() == "READY",
            "C batch cancelled")
    attempt = folder / "attempts" / uuid.uuid4().hex
    attempt.mkdir(parents=True)
    save(attempt / "request.json", dict(field_session=session, trials=rows))
    l = protocol["limits"]
    position = check_state(attempt / "pre.txt", l)
    for t in rows:
        clearance(l, position, t["start"], TRANSFER_CAP, l["min_stop"])
        clearance(l, t["start"], CENTER, MEAS_CAP, l["min_stop"])
        clearance(l, CENTER, t["end"], MEAS_CAP, l["min_stop"])
        position = t["end"]
    (attempt / "request.txt").write_text(
        " ".join(str(l[key]) for key in LIMIT_KEYS) + f" {len(rows)}\n" +
        "".join(f'{t["id"]} {t["history"]} {t["sign"]} {t["speed"]} {t["dwell"]}\n' for t in rows),
        encoding="ascii",
    )
    print("Independent precheck and route clearance passed.", flush=True)
    rc = 1
    try:
        with subprocess.Popen([str(EXE), "--run-batch", protocol["id"], attempt.name]) as child:
            while True:
                try:
                    rc = child.wait()
                    break
                except KeyboardInterrupt:
                    continue  # The native console receives Ctrl+C and requests a smooth stop.
    finally:
        save(attempt / "exit.json", dict(returncode=rc))
        check_state(attempt / "post.txt", l)
    known = replay(folder, protocol)
    return 0 if rc == 0 and all(t["id"] in known for t in rows) else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--plan", action="store_true")
    for name in ("freeze", "pending", "batch", "replay"):
        group.add_argument("--" + name, metavar="ID")
    parser.add_argument("--pilot", action="store_true", help="show or acquire the six pilot trials only")
    args = parser.parse_args()
    try:
        if args.plan:
            show([t for t in plan() if t["pilot"] or not args.pilot])
            return 0
        protocol_id = identifier(args.freeze or args.pending or args.batch or args.replay)
        folder = C / "manifest" / protocol_id
        if args.freeze:
            freeze(protocol_id, folder)
            return 0
        require(folder.is_dir(), "C protocol does not exist")
        with locked(folder):
            protocol = verified(protocol_id, folder)
            if args.batch:
                with locked(ROOT / "manifest/formal_a", "hardware.lock"):
                    return batch(folder, protocol, args.pilot)
            known = replay(folder, protocol, verify_sources=True)
            if args.pending:
                show([t for t in plan() if t["id"] not in known and (t["pilot"] or not args.pilot)])
    except (OSError, ValueError, KeyError, EOFError, KeyboardInterrupt) as error:
        parser.exit(1, f"C: {error or 'interrupted'}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
