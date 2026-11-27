"""Continue the authorized C scope one independently checked batch at a time."""

import argparse
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(root / "scripts"))

import c_run as c


def run_one(cmd, session, log):
    prompts = iter((
        ("Actual field session ID: ", session),
        ("Confirm current field authorization and safeguards. Type READY: ", "READY"),
        ("Type START to execute the displayed C batch: ", "START"),
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
                if "accepted" in buffer or "C:" in buffer or buffer.startswith("R"):
                    print(buffer.rstrip(), flush=True)
                buffer = ""
        if buffer:
            log.write(buffer + "\n")
            print(buffer, flush=True)
        code = child.wait()
    if code or pair:
        raise SystemExit(code or 1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("field_session", type=c.identifier)
    args = parser.parse_args()
    directory = Path(__file__).resolve().parent
    protocol = directory.name
    cli = [sys.executable, "-u", str(root / "scripts/c_run.py"), "--batch", protocol]
    with c.locked(directory, "supervisor.lock"), (directory / "run_console.txt").open(
            "a", encoding="utf-8", buffering=1) as log:
        frozen = c.verified(protocol, directory)
        known = c.replay(directory, frozen, verify_sources=True)
        while len(known) < len(frozen["trials"]):
            pending = [t for t in frozen["trials"] if t["id"] not in known]
            first = pending[0]
            message = f'{len(known)}/126 accepted; next {first["id"]} in {args.field_session}'
            print(message, flush=True)
            log.write(message + "\n")
            run_one(cli + (["--pilot"] if first["pilot"] else []), args.field_session, log)
            prior = len(known)
            known = c.replay(directory, frozen)
            if not 1 <= len(known) - prior <= 3:
                raise RuntimeError("Unexpected progress count")
        c.replay(directory, frozen, verify_sources=True)
    print("Authorized C scope complete: 126 trials", flush=True)


if __name__ == "__main__":
    main()
