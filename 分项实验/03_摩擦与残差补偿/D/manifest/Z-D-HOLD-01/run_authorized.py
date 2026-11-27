"""Continue the authorized D scope one independently checked batch at a time."""

import argparse
from pathlib import Path
import subprocess
import sys
from uuid import uuid4

root = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(root / "scripts"))

import d_run as d


def run_one(cmd, session, log):
    prompts = iter((
        ("Actual field session ID: ", session),
        ("Confirm current field authorization and safeguards. Type READY: ", "READY"),
        ("Type START to execute the displayed D batch: ", "START"),
    ))
    pair = next(prompts)
    cleared = False
    buffer = ""
    with subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True, encoding="utf-8", errors="replace", bufsize=1) as child:
        while char := child.stdout.read(1):
            buffer += char
            if pair and buffer.endswith(pair[0]):
                _, answer = pair
                if answer == "START" and not cleared:
                    child.stdin.close()
                    raise RuntimeError("Independent precheck evidence missing")
                log.write(buffer + answer + "\n")
                print(buffer + answer, flush=True)
                child.stdin.write(answer + "\n")
                child.stdin.flush()
                pair = next(prompts, None)
                buffer = ""
            elif char == "\n":
                log.write(buffer)
                if "Independent precheck and route clearance passed." in buffer:
                    cleared = True
                if "accepted" in buffer or "D:" in buffer or buffer.startswith("R"):
                    print(buffer.rstrip(), flush=True)
                buffer = ""
        if buffer:
            log.write(buffer + "\n")
            print(buffer, flush=True)
        code = child.wait()
    if code or pair:
        raise SystemExit(code or 1)



def recover_idle_check(directory, frozen, attempt):
    """Use a fresh independent snapshot only for the observed stationary ±1 pulse derivative."""
    native = attempt / "native.txt"
    before_motion = not any((attempt / name).exists() for name in
                            ("exit.json", "native.txt", "post.txt", "request.txt"))
    stage = "pre" if before_motion else "post"
    original_path = attempt / (stage + ".txt")
    if not original_path.is_file():
        return False
    if not before_motion and (not (attempt / "exit.json").is_file() or not native.is_file()
                             or d.load(attempt / "exit.json")["returncode"] != 0
                             or d.fields(native).get("outcome") != "complete"):
        return False
    snapshot = d.fields(original_path)
    limits = frozen["limits"]
    required = {"enabled":"1", "moving":"0", "fault":"0", "limit_plus":"0",
                "limit_minus":"0", "mode":"2", "planner":"0", "status_word":"1073742336"}
    if not (snapshot.get("ready") == "0" and snapshot.get("reason") == "not_idle_or_not_aligned"
            and abs(float(snapshot["velocity_pulse_s"])) == 20000
            and float(snapshot["planned_velocity_pulse_s"]) == 0
            and abs(float(snapshot["following_error_pulse"])) <= limits["follow"]
            and all(snapshot.get(k) == v for k, v in required.items())):
        return False
    with d.locked(directory), d.locked(root / "manifest/formal_a", "hardware.lock"):
        if (attempt / (stage + "_resolution.json")).exists() or (attempt / "acceptance.json").exists():
            return False
        fresh = []
        for _ in range(3):
            reread = attempt / (stage + "_recheck_" + uuid4().hex + ".txt")
            try:
                position = d.check_state(reread, limits)
            except ValueError:
                state = d.fields(reread)
                if not (state.get("ready") == "0" and state.get("reason") == "not_idle_or_not_aligned"
                        and abs(float(state["velocity_pulse_s"])) == 20000
                        and all(state.get(k) == v for k, v in required.items())):
                    return False
                continue
            state = d.fields(reread)
            if not (abs(position - float(snapshot["position_pulse"])) <= 10
                    and float(state["planned_pulse"]) == float(snapshot["planned_pulse"])
                    and float(state["planned_velocity_pulse_s"]) == 0
                    and all(state.get(k) == v for k, v in required.items())):
                return False
            fresh.append(reread)
            if len(fresh) == 2:
                break
        if len(fresh) != 2:
            return False
        original = original_path
        if not before_motion:
            original = attempt / "post_initial.txt"
            original.write_bytes(original_path.read_bytes())
        d.save(attempt / (stage + "_resolution.json"),
               dict(reason="isolated_stationary_encoder_velocity_sample",
                    initial={original.name: d.digest(original)},
                    independent_rereads={r.name: d.digest(r) for r in fresh},
                    accepted_snapshot=None if before_motion else fresh[-1].name))
        if not before_motion:
            replacement = attempt / "post_recheck_pending.txt"
            replacement.write_bytes(fresh[-1].read_bytes())
            replacement.replace(original_path)
        print(f"Independent {stage} rechecks passed at {position:g} pulse: {attempt.name}", flush=True)
        return stage



def attempt_ids(directory):
    return set((directory / "attempts").glob("*"))

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("field_session", type=d.identifier)
    parser.add_argument("--resolve-check", type=d.identifier)
    args = parser.parse_args()
    directory = Path(__file__).resolve().parent
    protocol = directory.name
    cli = [sys.executable, "-u", str(root / "scripts/d_run.py"), "--batch", protocol]
    with d.locked(directory, "supervisor.lock"), (directory / "run_console.txt").open(
            "a", encoding="utf-8", buffering=1) as log:
        frozen = d.verified(protocol, directory)
        if args.resolve_check:
            attempt = directory / "attempts" / args.resolve_check
            if not recover_idle_check(directory, frozen, attempt):
                raise SystemExit("Independent recheck did not pass")
        known = d.replay(directory, frozen, verify_sources=True)
        retry_counts = {}
        while len(known) < len(frozen["trials"]):
            pending = [t for t in frozen["trials"] if t["id"] not in known]
            first = pending[0]
            message = f'{len(known)}/18 accepted; next {first["id"]} in {args.field_session}'
            print(message, flush=True)
            log.write(message + "\n")
            before = attempt_ids(directory)
            try:
                run_one(cli + (["--pilot"] if first["pilot"] else []), args.field_session, log)
            except SystemExit:
                added = attempt_ids(directory) - before
                if len(added) != 1:
                    raise
                recovered = recover_idle_check(directory, frozen, added.pop())
                if not recovered:
                    raise
                if recovered == "pre":
                    retry_counts[first["id"]] = retry_counts.get(first["id"], 0) + 1
                    if retry_counts[first["id"]] > 2:
                        raise
                    continue
            prior = len(known)
            known = d.replay(directory, frozen)
            if not 1 <= len(known) - prior <= 3:
                raise RuntimeError("Unexpected progress count")
        d.replay(directory, frozen, verify_sources=True)
    print("Authorized D scope complete: 18 trials", flush=True)


if __name__ == "__main__":
    main()
