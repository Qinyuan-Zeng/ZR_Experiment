"""Plan, freeze, acquire and resume formal A batches on Windows."""
import argparse
import json
import math
import msvcrt
import re
import subprocess
import uuid
from contextlib import contextmanager
from pathlib import Path

from a0_replay import ROOT, MODEL, INERTIA, digest, require, write_csv
from a_dense_plan import SPEEDS, WINDOW, planned_pairs
from a_formal_analysis import analysis_grid, analyze, checked_state, endpoints, summarize

ALPHA = INERTIA/"results/d/Z-DP_20261126-095126_343_29884_summary.json"
EXE = ROOT/"bin/x64/Release/a_scan/ZRFrictionCurrent.exe"
PROTOCOLS = ROOT/"manifest/formal_a"
PAIRS = [dict(round=r, batch=(order-1)//4+1, pair=f"R{r}-V{v}", speed=v, first=first, split=split)
         for r, order, v, first, split in planned_pairs()]


def identifier(value):
    require(re.fullmatch(r"[A-Za-z0-9_-]{1,64}", value), "Use 1–64 ASCII letters, digits, hyphens or underscores")
    return value


def save(path, value):
    scratch = path.with_suffix(".tmp")
    scratch.write_text(json.dumps(value, indent=2, ensure_ascii=False, allow_nan=False), encoding="utf-8")
    scratch.replace(path)


def load(path):
    return json.loads(path.read_text(encoding="utf-8"))


@contextmanager
def locked(directory, name="protocol.lock"):
    with (directory/name).open("a+b") as handle:
        handle.seek(0)
        msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, 1)
        try:
            yield
        finally:
            handle.seek(0)
            msvcrt.locking(handle.fileno(), msvcrt.LK_UNLCK, 1)


def number(prompt, example=None):
    default = f" [{example}]" if example is not None else ""
    value = float(input(f"{prompt}{default}: ") or (str(example) if example is not None else "nan"))
    require(math.isfinite(value), "Non-finite or missing input")
    return value


def settings():
    p = dict(transfer=number("Confirmed transfer speed (mm/s)", 2),
             accel=number("Confirmed acceleration (pulse/s^2)", 400000),
             lo=number("Confirmed low turnaround (pulse)"),
             hi=number("Confirmed high turnaround (pulse)"),
             safe_lo=number("Verified lower protection boundary (pulse)"),
             safe_hi=number("Verified upper protection boundary (pulse)"),
             reserve=number("Site stopping allowance incl. response/uncertainty (pulse)"),
             follow=number("Maximum following error (pulse)"),
             min_stop=number("Verified minimum smooth-stop deceleration (pulse/ms^2)"))
    require(0 < p["transfer"] <= 5 and 0 < p["accel"] <= 400000 and
            0 <= p["lo"] < p["hi"] <= 32000 and p["lo"].is_integer() and p["hi"].is_integer() and
            p["safe_lo"] < p["lo"] < p["hi"] < p["safe_hi"] and
            p["safe_hi"] - p["safe_lo"] <= 40000 and
            min(p["reserve"], p["follow"], p["min_stop"]) > 0, "Inconsistent motion or protection limits")
    return p


def check_path(p, start, end, speed, smooth):
    if abs(start-end) <= 10:
        return
    need = p["reserve"] + p["follow"] + 10 + speed**2/(2*smooth*1e6)
    require(min(start,end)-p["safe_lo"] >= need and p["safe_hi"]-max(start,end) >= need,
            f"Insufficient stopping reserve [{start:g}, {end:g}] at {speed:g} pulse/s. Need {need:.1f} pulse each side")


def preview(p, position, grid, protect=False):
    require(0 <= position <= 32000, "Candidate position must be in command range 0–32000 pulse")
    require(0 <= p["lo"] < p["hi"] <= 32000 and 0 < p["accel"] <= 400000, "Invalid stroke or acceleration")
    print(f"17 speeds | 5 rounds | 85 pairs | 170 measurement legs | {len(grid)} analysis positions")
    print(f"Analysis window {grid[0]} to {grid[-1]} pulse")
    print(f"Turnarounds {p['lo']:g}, {p['hi']:g} pulse | accel {p['accel']:g} pulse/s^2")
    if not protect:
        print("Geometry only. Site stopping reserve has not been evaluated.")
    for speed in SPEEDS:
        crop = speed**2/(2*p["accel"]) + .05*speed
        low, high = p["lo"]+crop, p["hi"]-crop
        print(f"{speed:5d} pulse/s  nominal usable [{low:.1f}, {high:.1f}]")
        require(low <= grid[0] and high >= grid[-1], f"Predicted plateau too short at {speed} pulse/s")
        if protect:
            check_path(p, p["lo"], p["hi"], speed, p["min_stop"])
    for r in range(1,6):
        for b in range(1,6):
            rows = [row for row in PAIRS if (row["round"],row["batch"]) == (r,b)]
            changes = []
            for row in rows:
                start, _ = endpoints(p, row["first"])
                if abs(position-start) > 10:
                    changes.append(f"{position:g}->{start:g}")
                    if protect:
                        check_path(p, position, start, p["transfer"]*1000, p["min_stop"])
                position = start
            order = ", ".join(f'{row["speed"]}({"+" if row["first"] == 1 else "-"})' for row in rows)
            print(f"Round {r} batch {b}: {order} | transfers " + (", ".join(changes) or "none"))


def inputs():
    scripts = Path(__file__).parent
    return {"position": MODEL, "inertia": ALPHA, "executable": EXE, "sdk": EXE.with_name("gts.dll"),
            **{name: scripts/name for name in ("a_dense_plan.py","a0_replay.py","a_formal.py","a_formal_analysis.py")}}


def freeze(directory, protocol, candidate, grid):
    require(not directory.exists(), "Protocol ID already exists")
    inertia = load(ALPHA)
    alpha = float(inertia["alpha"])
    require(inertia["status"] == "passed" and inertia["accepted"] == inertia["planned"]
            and math.isfinite(alpha) and load(MODEL)["role"] == "B_primary", "Invalid frozen position or inertia model")
    hashes = {k: digest(path) for k,path in inputs().items()}
    p = settings()
    preview(p, number("Candidate batch-start position (pulse)", candidate), grid, protect=True)
    require(input(f"Type {protocol} to freeze: ") == protocol, "Freeze cancelled")
    directory.mkdir(parents=True)
    plan = directory/"plan.csv"
    write_csv(plan, [dict(row, leg=leg, direction=d, start_pulse=endpoints(p,d)[0], end_pulse=endpoints(p,d)[1])
                     for row in PAIRS for leg,d in enumerate((row["first"],-row["first"]),1)])
    save(directory/"protocol.json", dict(id=protocol, params=p, grid=grid.tolist(), pairs=PAIRS, alpha=alpha,
                                         hashes=hashes, plan_sha256=digest(plan)))
    (directory/"protocol.sha256").write_text(digest(directory/"protocol.json")+"\n", encoding="ascii")
    print(f"Frozen {protocol}: {plan}")


def verified(directory, protocol):
    protocol_file = directory/"protocol.json"
    require(digest(protocol_file) == (directory/"protocol.sha256").read_text(encoding="ascii").strip(),
            "Protocol parameters changed after freezing")
    frozen = load(protocol_file)
    require(frozen["id"] == protocol and frozen["grid"] and
            frozen["grid"] == analysis_grid(frozen["grid"][0]).tolist() and frozen["pairs"] == PAIRS
            and frozen["plan_sha256"] == digest(directory/"plan.csv"), "Plan differs from frozen protocol")
    for key,path in inputs().items():
        require(frozen["hashes"][key] == digest(path), f"Frozen input changed: {key}")
    return frozen


def state(attempt, label, p):
    snap = subprocess.run([str(EXE),"--state"], capture_output=True, text=True, encoding="ascii", errors="replace")
    record = attempt/(label+".txt")
    record.write_text(snap.stdout+snap.stderr, encoding="utf-8")
    require(snap.returncode == 0, f"{label} read-only check failed: {record}")
    return checked_state(record,p)


def all_attempts(directory):
    return sorted(p for p in (directory/"attempts").glob("*") if (p/"request.json").is_file())


def accepted_pairs(directory):
    accepted = {}
    for attempt in all_attempts(directory):
        result = attempt/"acceptance.json"
        if result.is_file():
            for row in load(result)["accepted"]:
                require(row["pair"] not in accepted, f'Multiple accepted attempts for {row["pair"]}')
                accepted[row["pair"]] = row
    return accepted


def pending(accepted):
    return [row for row in PAIRS if row["pair"] not in accepted]


def report(directory, frozen, accepted):
    summarize(directory, frozen["id"], accepted, PAIRS, frozen["grid"], load, save)


def recover(directory, frozen):
    accepted = accepted_pairs(directory)
    model = load(MODEL)
    for attempt in all_attempts(directory):
        if (attempt/"acceptance.json").exists() or not (attempt/"post.txt").exists():
            continue
        try:
            request = load(attempt/"request.json")
            result = analyze(frozen,attempt,request,model)
            result["accepted"] = [r for r in result["accepted"] if r["pair"] not in accepted]
            save(attempt/"acceptance.json",result)
            accepted.update({r["pair"]:r for r in result["accepted"]})
        except (KeyError,ValueError,OSError) as error:
            print(f"{attempt.name} remains unaccepted: {error}")
    report(directory,frozen,accepted)
    print(f"{len(accepted)}/85 pairs accepted")
    return accepted


def make_request(directory, queue):
    first = queue[0]
    rows = [r for r in queue if (r["round"],r["batch"]) == (first["round"],first["batch"])]
    print(f'Round {first["round"]} batch {first["batch"]}: '+", ".join(r["pair"] for r in rows))
    field_session = identifier(input("Actual field session ID: ").strip())
    if first["round"] == 5:
        require((directory/"models_frozen.json").is_file(), "Models must be frozen before round five")
        for path in all_attempts(directory):
            request = load(path/"request.json")
            same_round = request["pairs"][0]["round"] == 5
            same_session = request["field_session"].casefold() == field_session.casefold()
            require(same_round == same_session, "Round five needs its own single independent field session")
    require(input("Confirm field state and batch authorization. Type READY: ") == "READY", "Batch cancelled")
    return dict(pairs=rows,field_session=field_session)


def run_native(protocol, attempt):
    with subprocess.Popen([str(EXE),"--run-batch",protocol,attempt]) as running:
        while True:
            try:
                return running.wait()
            except KeyboardInterrupt:
                # Ctrl+C also reaches the console child, which requests a smooth stop.
                # Wait for it to close the card before launching the independent post-read.
                continue


def batch(directory, frozen, accepted):
    queue = pending(accepted)
    if not queue:
        print("Formal A complete: 85 accepted pairs")
        return True
    p = frozen["params"]
    request = make_request(directory,queue)
    rows = request["pairs"]
    attempt = directory/"attempts"/uuid.uuid4().hex
    attempt.mkdir(parents=True)
    save(attempt/"request.json",request)
    position,smooth = state(attempt,"pre",p)
    for row in rows:
        start,end = endpoints(p,row["first"])
        check_path(p,position,start,p["transfer"]*1000,smooth)
        check_path(p,start,end,row["speed"],smooth)
        position = start
    print("Independent pre-check passed. Transfer and scan stopping margins passed.")
    config = [p["transfer"],p["accel"]/1000,p["lo"]/1000,p["hi"]/1000,p["safe_lo"],p["safe_hi"],
              p["reserve"],p["follow"],p["min_stop"],frozen["grid"][0],rows[0]["round"],len(rows)]
    (attempt/"batch.txt").write_text(" ".join(map(str,config))+"\n"+
                                    "".join(f'{r["speed"]} {r["first"]} {r["split"]}\n' for r in rows),encoding="ascii")
    rc = None
    try:
        rc = run_native(frozen["id"],attempt.name)
    finally:
        save(attempt/"exit.json",{"returncode":rc})
        state(attempt,"post",p)
    result = analyze(frozen,attempt,request,load(MODEL))
    save(attempt/"acceptance.json",result)
    for key,reason in result["rejected"].items():
        print(f"Pending {key}: {reason}")
    print(f'{len(result["accepted"])}/{len(rows)} pairs accepted | {attempt.name}')
    report(directory,frozen,accepted_pairs(directory))
    return rc == 0 and not result["rejected"]


def menu():
    actions = {"2": "--state", "3": "--freeze", "4": "--batch", "5": "--pending", "6": "--replay"}
    while True:
        print("\n正式 A 恒速摩擦实验")
        print("1. 离线预览   2. 只读状态   3. 冻结协议")
        print("4. 执行一批   5. 查看待采   6. 离线回放   0. 退出")
        try:
            choice = input("请选择：").strip()
            if choice == "0":
                return 0
            if choice == "1":
                position = number("候选起点（pulse）", 0)
                lo = int(input("低换向点（pulse） [0]: ") or "0")
                hi = int(input("高换向点（pulse） [32000]: ") or "32000")
                accel = number("加减速度（pulse/s^2）", 400000)
                args = ["--plan", "--position", str(position), "--lo", str(lo),
                        "--hi", str(hi), "--accel", str(accel)]
            elif choice in actions:
                args = [actions[choice]]
                if choice != "2":
                    args.append(identifier(input("协议 ID：").strip()))
            else:
                print("请输入菜单编号 0～6。")
                continue
            if main(args):
                print("操作未通过，请核对上方输出。")
        except (EOFError, KeyboardInterrupt):
            return 0
        except SystemExit as error:
            if error.code:
                print("操作未通过，请核对上方输出。")
        except ValueError as error:
            print(error)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    for name in ("freeze","pending","batch","replay"):
        mode.add_argument("--"+name,metavar="PROTOCOL")
    mode.add_argument("--plan",action="store_true")
    mode.add_argument("--state",action="store_true")
    mode.add_argument("--menu",action="store_true")
    parser.add_argument("--position",type=float,help="candidate initial position in pulses")
    parser.add_argument("--lo",type=int,default=0,help="preview low turnaround, pulses")
    parser.add_argument("--hi",type=int,default=32000,help="preview high turnaround, pulses")
    parser.add_argument("--accel",type=float,default=400000,help="preview acceleration, pulses/s^2")
    parser.add_argument("--grid-lo", type=int, default=WINDOW[0],
                        help="formal A analysis start in pulses (plan and freeze)")
    args = parser.parse_args(argv)
    if args.menu:
        return menu()
    try:
        if args.plan:
            preview(dict(lo=args.lo,hi=args.hi,accel=args.accel),
                    0 if args.position is None else args.position, analysis_grid(args.grid_lo))
            return 0
        if args.state:
            PROTOCOLS.mkdir(parents=True,exist_ok=True)
            with locked(PROTOCOLS,"hardware.lock"):
                return subprocess.call([str(EXE),"--state"])
        protocol = identifier(args.freeze or args.pending or args.batch or args.replay)
        directory = PROTOCOLS/protocol
        if args.freeze:
            freeze(directory,protocol,args.position,analysis_grid(args.grid_lo))
            return 0
        with locked(directory):
            frozen = verified(directory,protocol)
            if args.replay:
                recover(directory,frozen)
            elif args.pending:
                queue = pending(accepted_pairs(directory))
                for r in queue:
                    print(f'{r["pair"]} batch {r["batch"]} {r["split"]}')
                print(f"{len(queue)}/85 pending")
            else:
                with locked(PROTOCOLS,"hardware.lock"):
                    return 0 if batch(directory,frozen,recover(directory,frozen)) else 1
    except (KeyError,ValueError,OSError,EOFError,KeyboardInterrupt) as error:
        parser.exit(1,f"Formal A: {error or 'interrupted'}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
