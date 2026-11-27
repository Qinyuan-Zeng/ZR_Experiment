"""Offline B1 point table and checked, resumable acquisition console."""
import argparse
import csv
import math
from pathlib import Path
import subprocess
import uuid

import pandas as pd

from a_formal import identifier, locked, load, save
from a0_replay import digest, require
from b1_replay import check_pair

ROOT = Path(__file__).resolve().parents[1]
B1 = ROOT / "B1"
MANIFEST = B1 / "manifest"
EXE = ROOT / "bin/x64/Release/b1_scan/ZRFrictionCurrent.exe"
GAPS = ROOT / "B0/gap_matrix.csv"
CAPTURE = ROOT / "src/b1_scan.cpp"
STAGE2 = ROOT.parent / "02_等效惯性电流辨识/include"
PILOT = [(10, 400), (20, 400), (20, 600), (40, 400), (40, 600)]
LIMIT_NAMES = ("lo", "hi", "reserve", "follow", "min_stop", "max_speed")


def plan_rows():
    with GAPS.open(encoding="utf-8-sig", newline="") as f:
        cells = [r for r in csv.DictReader(f) if r["stage"] == "B1"]
    conditions = [(r["path"], int(r["start_mm"])*1000, int(r["end_mm"])*1000,
                   int(r["target_speed_mm_s"]), int(r["target_accel_mm_s2"])) for r in cells]
    require(len(conditions) == len(set(conditions)) == 32, "B0 B1 matrix is incomplete or duplicated")
    pilot = [next(c for c in conditions if (c[0], c[3], c[4]) == (path, v, a))
             for v, a in PILOT for path in ("P1", "P2")]
    # The first round contains the pilot as its first ten entries, not additional runs.
    ladder = [(2, 400), (40, 600), (5, 400), (30, 500), (10, 400), (20, 600),
              (10, 600), (40, 400), (15, 400), (30, 600), (20, 400), (40, 500),
              (15, 500), (30, 400), (15, 600), (20, 500)]
    interleaved = [
        next(c for c in conditions if (c[0], c[3], c[4]) == (path, v, a))
        for i, (v, a) in enumerate(ladder)
        for path in (("P1", "P2") if i % 2 == 0 else ("P2", "P1"))
    ]
    require(set(interleaved) == set(conditions), "Scheduled conditions differ from B0")
    rows = []
    for r in range(1, 6):
        shift = (r-1)*7 % 32
        order = interleaved[shift:] + interleaved[:shift]
        if r == 1:
            order = pilot + [c for c in order if c not in pilot]
        for i, (path, lo, hi, v, a) in enumerate(order):
            is_pilot = r == 1 and i < len(pilot)
            rows.append(dict(id=f"R{r}-{path}-V{v}-A{a}", round=r, period="S1" if r <= 3 else "S2",
                             path=path, lo=lo, hi=hi, v=v, a=a, first=(-1 if (conditions.index((path, lo, hi, v, a))+r) % 2 else 1),
                             pilot=is_pilot, ideal_ramp_ms=v/a*1000,
                             ideal_cruise_mm=(hi-lo)/1000-v*v/a))
    require(len(rows) == len({p["id"] for p in rows}) == 160, "B1 slot table is not unique")
    require(all(r["ideal_cruise_mm"] > 0 for r in rows), "B1 planned ramp distance exceeds stroke")
    return rows


def show(rows, pilot_only=False):
    shown = [p for p in rows if p["pilot"]] if pilot_only else rows
    scope = "pilot" if pilot_only else "32 conditions x 5" if len(shown)==160 else "selected slots"
    print(f"{len(shown)} bidirectional pairs | {2*len(shown)} legs | {scope}")
    print("P1 10000-22000 pulse | P2 12000-20000 pulse | pre/post settled hold 1 s each")
    print("Existing TRAP clock readback 50 us required. Watch 1/core tick; no interpolation setter.")
    for p in shown:
        start,end = (p["lo"],p["hi"]) if p["first"]==1 else (p["hi"],p["lo"])
        print(f'{p["id"]:19} {p["period"]}  {start:5} -> {end:5} -> {start:5} pulse'
              f'  {p["v"]:2} mm/s  {p["a"]} mm/s^2  cruise {p["ideal_cruise_mm"]:5.2f} mm')


def number(prompt, default=None):
    value = input(prompt + (f" [{default}]" if default is not None else "") + ": ").strip()
    require(value or default is not None, "A field-confirmed value is required")
    result = float(value or default)
    require(math.isfinite(result), "A finite number is required")
    return result


def limits():
    answers = (number("Lower approved protection boundary (pulse)",0),
               number("Upper approved protection boundary (pulse)",37000),
               number("Verified additional stopping reserve (pulse)",4000),
               number("Verified following error limit (pulse)",500),
               number("Verified minimum smooth stop deceleration (pulse/ms^2)",.99),
               number("Verified scan speed protection limit (mm/s)",50))
    l = dict(zip(LIMIT_NAMES, answers))
    require(0 <= l["lo"] < 10000 and 22000 < l["hi"] <= 37000 and l["reserve"] >= 1
            and 1 <= l["follow"] <= 500 and l["min_stop"] > 0 and 40 < l["max_speed"] <= 60,
            "B1 protection value outside supported range")
    return l


def fingerprints():
    paths = (GAPS, EXE, EXE.with_name("gts.dll"), CAPTURE, STAGE2 / "matched_motion.hpp",
             STAGE2 / "matched_batch.hpp", STAGE2 / "watch_capture.hpp",
             Path(__file__).resolve(), Path(__file__).with_name("b1_replay.py"))
    return {str(p.relative_to(ROOT.parent)): digest(p) for p in paths}


def clearance(l, p, q, cap, stop):
    required = l["reserve"] + l["follow"] + 10 + (cap*1000)**2/(2*stop*1e6)
    available = min(min(p,q)-l["lo"],l["hi"]-max(p,q))
    require(available >= required,
            f"Stopping reserve {p:g}->{q:g}: available {available:.1f}, need {required:.1f} pulse")


def freeze(protocol, directory):
    require(not directory.exists(), "B1 protocol ID already exists")
    rows = plan_rows()
    hashes = fingerprints()
    l = limits()
    for row in rows:
        clearance(l,row["lo"],row["hi"],l["max_speed"],l["min_stop"])
    show(rows, pilot_only=True)
    require(input(f"Type {protocol} to freeze B1: ").strip()==protocol, "B1 freeze cancelled")
    directory.mkdir(parents=True)
    point_table=directory/"plan.csv"
    with point_table.open("w",encoding="utf-8-sig",newline="") as f:
        writer=csv.DictWriter(f,fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    save(directory/"protocol.json",dict(id=protocol,slots=rows,limits=l,hashes=hashes,
                                        plan_sha256=digest(point_table),core_period_ns=50000,
                                        watch_expected_frame_ns=50000,host_poll_request_ms=5,analysis_grid_ms=5))
    (directory/"protocol.sha256").write_text(digest(directory/"protocol.json")+"\n",encoding="ascii")
    print(f"Frozen {protocol}: 160 pairs. Pilot is the first 10 pairs of S1.")


def verified(protocol,directory):
    source=directory/"protocol.json"
    require(digest(source)==(directory/"protocol.sha256").read_text(encoding="ascii").strip(),
            "Frozen B1 protocol changed")
    frozen=load(source)
    require(frozen["id"]==protocol and frozen["slots"]==plan_rows() and frozen["hashes"]==fingerprints()
            and frozen["plan_sha256"]==digest(directory/"plan.csv"),
            "Frozen B1 schedule, binary or source changed")
    return frozen


def fields(file):
    return dict(row.split("=",1) for row in file.read_text(encoding="utf-8-sig").splitlines() if "=" in row)


def read_state(file,l):
    s=fields(file)
    x=float(s["position_pulse"])
    require(s["ready"]=="1" and 0 <= x <= 32000 and l["lo"]<=x<=l["hi"] and
            abs(float(s["following_error_pulse"]))<=l["follow"],
            "B1 independent state differs from permitted configuration")
    return x


def capture_state(file,l):
    r=subprocess.run([str(EXE),"--state"],capture_output=True,text=True,encoding="ascii",errors="replace")
    file.write_text(r.stdout+r.stderr,encoding="utf-8")
    require(r.returncode==0, f"B1 independent state check failed: {file}")
    return read_state(file,l)


def attempts(directory):
    return sorted(p for p in (directory/"attempts").glob("*") if (p/"request.json").is_file())


def accepted(directory, verify_recorded=False):
    outcome={}
    for attempt in attempts(directory):
        f=attempt/"acceptance.json"
        if f.is_file():
            record=load(f)
            if verify_recorded and record["accepted"]:
                for field in ("pre","post","native","request"):
                    extension=".json" if field=="request" else ".txt"
                    require(digest(attempt/(field+extension))==record[field+"_sha256"],
                            f"B1 accepted batch artifact changed: {field}")
            for r in record["accepted"]:
                if verify_recorded:
                    for leg in r["leg_inputs"].values():
                        for source in leg.values():
                            require(digest(Path(source["path"]))==source["sha256"],
                                    f'B1 accepted source changed: {source["path"]}')
                require(r["slot_id"] not in outcome,"Duplicate slot in B1 ledger")
                outcome[r["slot_id"]]=r
    return outcome


def replay(directory,frozen,verify_recorded=False):
    known=accepted(directory,verify_recorded)
    l=frozen["limits"]
    protocol=frozen["id"]
    rows={x["id"]:x for x in frozen["slots"]}
    for attempt in attempts(directory):
        outcome=attempt/"acceptance.json"
        if outcome.is_file() or not (attempt/"post.txt").is_file():
            continue
        try:
            read_state(attempt/"pre.txt",l)
            read_state(attempt/"post.txt",l)
            request=load(attempt/"request.json")
            require(all(rows[s["id"]]==s for s in request["slots"]),"Acquired B1 slots differ from frozen schedule")
            native=fields(attempt/"native.txt")
            require(native["core_period_ns"]==str(frozen["core_period_ns"]), "Clock changed")
            folder=B1/"data/raw"/protocol/attempt.name
            result=dict(accepted=[],rejected={},
                        pre_sha256=digest(attempt/"pre.txt"),post_sha256=digest(attempt/"post.txt"),
                        native_sha256=digest(attempt/"native.txt"),request_sha256=digest(attempt/"request.json"),
                        field_session=request["field_session"],
                        attempt_id=attempt.name)
            for slot in request["slots"]:
                if slot["id"] in known:
                    continue
                try:
                    data=check_pair(folder,slot,l)
                    item=dict(slot_id=slot["id"],field_session=request["field_session"],attempt_id=attempt.name,**data)
                    result["accepted"].append(item)
                    known[slot["id"]]=item
                except (FileNotFoundError,ValueError,KeyError,pd.errors.EmptyDataError) as e:
                    result["rejected"][slot["id"]]=str(e)
                    print(f'{slot["id"]} remains pending: {e}')
            save(outcome,result)
        except (OSError,ValueError,KeyError,TypeError) as error:
            print(f"{attempt.name}: {error}")
    print(f"{len(known)}/160 B1 pairs accepted")
    if known:
        output=B1/"results"/protocol
        output.mkdir(parents=True,exist_ok=True)
        clean=[{k:v for k,v in row.items() if k != "leg_inputs"} for row in known.values()]
        pd.DataFrame(clean).to_csv(output/"accepted_pairs.csv",index=False,encoding="utf-8-sig")
    return known


def queue_for(frozen,known,pilot=False):
    return [r for r in frozen["slots"] if r["id"] not in known and (not pilot or r["pilot"])]


def batch(directory,frozen,pilot):
    known=replay(directory,frozen)
    queue=queue_for(frozen,known,pilot)
    if not queue:
        print("B1 pilot complete" if pilot else "B1 complete")
        return 0
    first=queue[0]
    batch_slots=[r for r in queue if r["round"]==first["round"] and r["pilot"]==first["pilot"]][:4]
    show(batch_slots)
    label=identifier(input("Actual field session ID: ").strip())
    if first["period"]=="S2":
        require(all(r["field_session"].casefold()!=label.casefold() for r in known.values()
                    if r["period"]=="S1"),"S2 must use a field session distinct from S1")
    require(input("Confirm field state, authorized scope and safeguards (including existing drive protection). Type READY: ").strip()=="READY",
            "B1 batch cancelled")
    attempt=directory/"attempts"/uuid.uuid4().hex
    attempt.mkdir(parents=True)
    save(attempt/"request.json",dict(slots=batch_slots,field_session=label))
    l=frozen["limits"]
    position=capture_state(attempt/"pre.txt",l)
    for s in batch_slots:
        start=s["lo"] if s["first"]==1 else s["hi"]
        if abs(position-start)>10:
            clearance(l,position,start,5,l["min_stop"])
        clearance(l,s["lo"],s["hi"],l["max_speed"],l["min_stop"])
        position=start
    (attempt/"request.txt").write_text(
        " ".join(str(l[k]) for k in LIMIT_NAMES)+f" {len(batch_slots)}\n"+
        "".join(f'{s["id"]} {s["lo"]} {s["hi"]} {s["v"]} {s["a"]} {s["first"]}\n' for s in batch_slots),
        encoding="ascii")
    print("Independent precheck and route clearance passed.")
    rc=1
    try:
        with subprocess.Popen([str(EXE),"--run-batch",frozen["id"],attempt.name]) as running:
            while True:
                try:
                    rc=running.wait()
                    break
                except KeyboardInterrupt:
                    continue
    finally:
        save(attempt/"exit.json",dict(returncode=rc))
        capture_state(attempt/"post.txt",l)
    known=replay(directory,frozen)
    return 0 if rc==0 and all(r["id"] in known for r in batch_slots) else 1


def main():
    p=argparse.ArgumentParser(description=__doc__)
    group=p.add_mutually_exclusive_group(required=True)
    group.add_argument("--plan",action="store_true")
    group.add_argument("--state",action="store_true")
    for option in ("freeze","pending","batch","replay"):
        group.add_argument("--"+option,metavar="ID")
    p.add_argument("--pilot",action="store_true",help="only show or acquire the first ten pairs")
    a=p.parse_args()
    try:
        if a.plan:
            show(plan_rows(),pilot_only=a.pilot)
            return 0
        if a.state:
            global_lock=ROOT/"manifest/formal_a"
            global_lock.mkdir(exist_ok=True)
            with locked(global_lock,"hardware.lock"):
                return subprocess.call([str(EXE),"--state"])
        protocol=identifier(a.freeze or a.pending or a.batch or a.replay)
        directory=MANIFEST/protocol
        if a.freeze:
            freeze(protocol,directory)
            return 0
        with locked(directory):
            frozen=verified(protocol,directory)
            if a.pending:
                show(queue_for(frozen,accepted(directory),a.pilot))
            elif a.replay:
                replay(directory,frozen,verify_recorded=True)
            else:
                global_lock=ROOT/"manifest/formal_a"
                global_lock.mkdir(exist_ok=True)
                with locked(global_lock,"hardware.lock"):
                    return batch(directory,frozen,a.pilot)
    except (OSError,ValueError,KeyError,TypeError,EOFError,KeyboardInterrupt) as e:
        p.exit(1,f"B1: {e or 'interrupted'}\n")
    return 0


if __name__=="__main__":
    raise SystemExit(main())
