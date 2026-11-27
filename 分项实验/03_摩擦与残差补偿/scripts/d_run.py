"""Stage 3 D HOLD acquisition and offline quality replay."""
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
D = ROOT / "D"
EXE = ROOT / "bin/x64/Release/d_hold/ZRFrictionCurrent.exe"
HOLD_MS = 6000
STROKE = 4000
PERIOD_NS = 50_000
LIMIT_KEYS = ("lo", "hi", "reserve", "follow", "min_stop")
MEAS_CAP = 15
TRANSFER_CAP = 5


def plan():
    rows = []
    for repeat in (1, 2, 3):
        positions = (12000, 16000, 20000) if repeat != 2 else (20000, 16000, 12000)
        for target, sign in product(positions, (1, -1) if repeat != 2 else (-1, 1)):
            direction = "P" if sign == 1 else "N"
            rows.append(dict(id=f"R{repeat}-{target}-{direction}", repeat=repeat, sign=sign,
                             target=target, start=target - sign * STROKE,
                             pilot=repeat == 1 and target == 12000))
    return rows


def show(rows):
    print(f"{len(rows)} D holds | hold {HOLD_MS} ms | approach 10 mm/s, 400 mm/s^2 | Watch 50 us")
    for row in rows:
        print(f'{row["id"]:18} {row["start"]} -> {row["target"]} pulse')


def fingerprints():
    inc = ROOT.parent / "02_等效惯性电流辨识/include"
    paths = [Path(__file__), EXE, EXE.with_name("gts.dll"), ROOT / "src/d_hold.cpp",
             ROOT / "scripts/b1_run.py", ROOT / "scripts/a_formal.py", ROOT / "scripts/a0_replay.py",
             ROOT.parent / "01_位置相关空载电流辨识/models/position_B_primary_v1.json",
             *(inc / name for name in ("matched_batch.hpp", "matched_motion.hpp", "watch_capture.hpp"))]
    return {p.relative_to(ROOT.parent).as_posix(): digest(p) for p in paths}


def freeze(protocol, folder):
    require(not folder.exists(), "D protocol ID already exists")
    l = dict(zip(LIMIT_KEYS, (
        number("Protection lower pulse", 0), number("Protection upper pulse", 37000),
        number("Additional stopping reserve pulse", 4000), number("Following error limit pulse", 500),
        number("Minimum smooth stop pulse/ms^2", .99),
    )))
    require(0 <= l["lo"] < 8000 and 24000 < l["hi"] <= 37000 and l["reserve"] >= 1
            and 1 <= l["follow"] <= 500 and l["min_stop"] > 0, "Unsupported D protection values")
    clearance(l, 8000, 24000, MEAS_CAP, l["min_stop"])
    rows = plan()
    hashes = fingerprints()
    show([row for row in rows if row["pilot"]])
    require(input(f"Type {protocol} to freeze D: ").strip() == protocol, "D freeze cancelled")
    folder.mkdir(parents=True)
    table = folder / "plan.csv"
    with table.open("w", encoding="utf-8-sig", newline="") as file:
        writer = csv.DictWriter(file, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    manifest = folder / "protocol.json"
    save(manifest, dict(id=protocol, limits=l, trials=rows, hashes=hashes, plan_sha256=digest(table),
                        core_period_ns=PERIOD_NS, hold_ms=HOLD_MS, measurement_cap=MEAS_CAP, transfer_cap=TRANSFER_CAP))
    (folder / "protocol.sha256").write_text(digest(manifest) + "\n", encoding="ascii")
    print(f"{protocol} frozen offline | 18 trials including 2 pilot trials")


def verified(protocol, folder):
    path = folder / "protocol.json"
    require(digest(path) == (folder / "protocol.sha256").read_text().strip(), "D protocol hash mismatch")
    p = load(path)
    require(p["id"] == protocol and p["trials"] == plan() and p["hashes"] == fingerprints() and
            p["plan_sha256"] == digest(folder / "plan.csv"), "D frozen sources or point table changed")
    return p


def check_trial(raw_dir, trial, limits):
    paths = {ext: raw_dir / (trial["id"] + ext) for ext in (".txt", ".csv", ".sdk.csv", ".batches.csv")}
    hashes = {ext: digest(path) for ext, path in paths.items()}
    m = fields(paths[".txt"])
    require(m["schema"] == "d_hold_v1" and m["slot_id"] == trial["id"] and
            int(m["direction"]) == trial["sign"] and int(m["start_pulse"]) == trial["start"] and
            int(m["target_pulse"]) == trial["target"] and int(m["hold_ms"]) == HOLD_MS and
            int(m["core_period_ns"]) == PERIOD_NS and m["outcome"] == "complete" and
            m["watch_aligned_contiguous"] == "1", "Incomplete D capture or mismatched configuration")
    raw = pd.read_csv(paths[".csv"])
    require(list(raw.columns) == ["frame", "card_tick_raw", "encoder_position_raw", "current_feedback_raw"]
            and len(raw) > 120000 and np.isfinite(raw.to_numpy(float)).all(), "Invalid D Watch shape")
    ticks = raw.card_tick_raw.to_numpy(dtype=np.int64)
    offsets = (ticks - ticks[0]) % (1 << 32)
    require(np.all(np.diff(offsets) == 1) and np.array_equal(raw.frame, np.arange(len(raw))),
            "Incomplete D Watch ticks")
    events = ("setup_hold_begin", "setup_hold_end", "approach_before", "approach_after",
              "approach_settled", "hold_begin", "hold_end")
    e = {name: (int(m[name + "_tick"]) - int(ticks[0])) % (1 << 32) for name in events}
    require(list(e.values()) == sorted(e.values()) and e["hold_end"] <= offsets[-1], "D event order error")
    x = raw.encoder_position_raw.to_numpy()
    start, target = trial["start"], trial["target"]
    require(x.min() >= max(limits["lo"], min(start, target) - 10) and
            x.max() <= min(limits["hi"], max(start, target) + 10), "D Watch position outside path")
    for begin, end, goal, ms in (("setup_hold_begin", "setup_hold_end", start, 250),
                                 ("hold_begin", "hold_end", target, HOLD_MS)):
        z = x[e[begin]:e[end] + 1]
        require(len(z) * PERIOD_NS / 1e6 >= ms and np.abs(z - goal).max() <= 10 and np.ptp(z) <= 10,
                f"D {begin} failed duration or position stability")
    require(np.abs(x[e["approach_settled"]:e["hold_end"] + 1] - target).max() <= 10,
            "D position left HOLD tolerance after arrival")
    segment = x[e["approach_after"]:e["approach_settled"] + 1]
    lag = 100
    require(len(segment) > lag, "D approach not observed")
    v = trial["sign"] * (segment[lag:] - segment[:-lag]) / 5
    require(abs(segment[0] - start) <= 10 and abs(segment[-1] - target) <= 10 and
            np.abs(v).max() <= MEAS_CAP and np.percentile(v, 95) >= 9, "D approach speed or endpoints failed")
    host = pd.read_csv(paths[".sdk.csv"])
    codes = [key for key in host if key.startswith("rc_")]
    status = host.status.to_numpy(dtype=np.int64)
    require(len(codes) == 9 and host[codes].eq(0).all(axis=None) and
            {"setup_hold", "approach", "hold"} <= set(host.phase) and
            host.host_begin_us.diff().dropna().gt(0).all() and
            np.all((status & ((1 << 1) | (1 << 5) | (1 << 6))) == 0) and np.all(status & (1 << 9)) and
            np.abs(host.following_error_pulse).max() <= limits["follow"],
            "D invalid host trace or unsafe state recorded")
    require(all(digest(path) == hashes[ext] for ext, path in paths.items()), "D sources changed during replay")
    return dict(frames=len(raw), watch_period_us=PERIOD_NS / 1000,
                arrival_to_hold_end_ms=(e["hold_end"] - e["approach_settled"]) * PERIOD_NS / 1e6,
                guaranteed_hold_ms=(e["hold_end"] - e["hold_begin"]) * PERIOD_NS / 1e6,
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
                        f"D batch evidence changed: {attempt.name}")
        else:
            l = protocol["limits"]
            try:
                read_state(attempt / "pre.txt", l)
                read_state(attempt / "post.txt", l)
                require(fields(attempt / "native.txt")["core_period_ns"] == str(PERIOD_NS), "D clock mismatch")
                request = load(attempt / "request.json")
                require(all(trials[t["id"]] == t for t in request["trials"]), "D executed point table differs")
            except (OSError, ValueError, KeyError) as error:
                print(f"{attempt.name}: {error}")
                continue
            evidence = dict(accepted=[], rejected={}, batch={
                name: digest(attempt / name) for name in ("pre.txt", "post.txt", "request.json", "request.txt", "exit.json", "native.txt")})
            for trial in request["trials"]:
                if trial["id"] in known:
                    continue
                try:
                    data = check_trial(D / "data/raw" / protocol["id"] / attempt.name, trial, l)
                    evidence["accepted"].append(dict(trial, **data, field_session=request["field_session"],
                                                      attempt=attempt.name))
                except (OSError, ValueError, KeyError) as error:
                    evidence["rejected"][trial["id"]] = str(error)
                    print(f'{trial["id"]}: {error}')
            save(result, evidence)
        for row in evidence["accepted"]:
            require(row["id"] not in known, "Duplicate D accepted trial")
            if verify_sources:
                require(all(digest(Path(name)) == sha for name, sha in row["inputs"].items()), "D raw evidence changed")
            known[row["id"]] = row
    print(f"{len(known)}/{len(trials)} D trials accepted")
    if known:
        out = D / "results" / protocol["id"]
        out.mkdir(parents=True, exist_ok=True)
        temp = out / "accepted_trials.tmp"
        pd.DataFrame([{k: v for k, v in known[t].items() if k != "inputs"}
                      for t in trials if t in known]).to_csv(temp, index=False, encoding="utf-8-sig")
        temp.replace(out / "accepted_trials.csv")
    return known


def check_state(path, limits):
    r = subprocess.run([str(EXE), "--state"], capture_output=True, text=True, encoding="ascii", errors="replace")
    path.write_text(r.stdout + r.stderr, encoding="utf-8")
    require(r.returncode == 0, f"D independent state check failed: {path}")
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
            "D batch cancelled")
    attempt = folder / "attempts" / uuid.uuid4().hex
    attempt.mkdir(parents=True)
    save(attempt / "request.json", dict(field_session=session, trials=rows))
    l = protocol["limits"]
    position = check_state(attempt / "pre.txt", l)
    for t in rows:
        clearance(l, position, t["start"], TRANSFER_CAP, l["min_stop"])
        clearance(l, t["start"], t["target"], MEAS_CAP, l["min_stop"])
        position = t["target"]
    (attempt / "request.txt").write_text(
        " ".join(str(l[key]) for key in LIMIT_KEYS) + f" {len(rows)}\n" +
        "".join(f'{t["id"]} {t["target"]} {t["sign"]}\n' for t in rows),
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
    parser.add_argument("--pilot", action="store_true", help="show or acquire the two pilot trials only")
    args = parser.parse_args()
    try:
        if args.plan:
            show([t for t in plan() if t["pilot"] or not args.pilot])
            return 0
        protocol_id = identifier(args.freeze or args.pending or args.batch or args.replay)
        folder = D / "manifest" / protocol_id
        if args.freeze:
            freeze(protocol_id, folder)
            return 0
        require(folder.is_dir(), "D protocol does not exist")
        with locked(folder):
            protocol = verified(protocol_id, folder)
            if args.batch:
                with locked(ROOT / "manifest/formal_a", "hardware.lock"):
                    return batch(folder, protocol, args.pilot)
            known = replay(folder, protocol, verify_sources=True)
            if args.pending:
                show([t for t in plan() if t["id"] not in known and (t["pilot"] or not args.pilot)])
    except (OSError, ValueError, KeyError, EOFError, KeyboardInterrupt) as error:
        parser.exit(1, f"D: {error or 'interrupted'}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
