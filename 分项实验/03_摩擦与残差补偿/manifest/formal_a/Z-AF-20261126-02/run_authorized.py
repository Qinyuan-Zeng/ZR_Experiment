import os
from pathlib import Path
import re
import subprocess
import sys

root = Path.cwd() / "分项实验/03_摩擦与残差补偿"
script = root / "scripts/a_formal.py"
protocol = "Z-AF-20261126-02"
directory = root / "manifest/formal_a" / protocol
env = dict(os.environ, PYTHONIOENCODING="utf-8")
base = [sys.executable, "-u", str(script)]
limit = int(sys.argv[1])
log_path = directory / "run_console.txt"

def next_batch():
    result = subprocess.run(base + ["--pending", protocol], capture_output=True, text=True,
                            encoding="utf-8", check=True, env=env)
    match = re.search(r"(?m)^R(\d+)-V\d+ batch (\d+) (\w+)$", result.stdout)
    counts = re.search(r"(?m)^(\d+)/85 pending$", result.stdout)
    if counts is None or (bool(match) != bool(int(counts[1]))):
        raise RuntimeError("Cannot verify the next frozen batch")
    return (int(counts[1]), int(match[1]), int(match[2])) if match else (0, 0, 0)

def capture_batch(round_number, field_session, log):
    prompts = [
        ("Actual field session ID: ", field_session),
        ("Confirm field state and batch authorization. Type READY: ", "READY"),
        ("Confirm this authorized batch. Type START: ", "START"),
    ]
    precheck = False
    line = ""
    with subprocess.Popen(base + ["--batch", protocol], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace",
                          env=env, bufsize=1) as process:
        while True:
            char = process.stdout.read(1)
            if not char:
                break
            line += char
            if prompts and line.endswith(prompts[0][0]):
                prompt, reply = prompts.pop(0)
                if reply == "START" and not precheck:
                    process.stdin.close()
                    raise RuntimeError("Refused to start without independent pre-check")
                log.write(line+reply+"\n")
                log.flush()
                print(line+reply, flush=True)
                process.stdin.write(reply+"\n")
                process.stdin.flush()
                line = ""
            if char == "\n":
                log.write(line)
                log.flush()
                if "Independent pre-check passed" in line:
                    precheck = True
                if any(word in line for word in ("accepted", "Session ", "Formal A:", "outcome", "axis_", "mismatch",
                                                "Independent pre-check", "stop", "Samples ", "Pending ")):
                    print(line.strip(), flush=True)
                line = ""
        if line:
            log.write(line+"\n")
            print(line, flush=True)
        exit_code = process.wait()
        if exit_code or prompts:
            raise SystemExit(exit_code or 1)

with log_path.open("a", encoding="utf-8", buffering=1) as log:
    for _ in range(limit):
        pending, round_number, batch_number = next_batch()
        if not pending:
            print("All 85 pairs have passed.", flush=True)
            break
        label = "Z-AF-20261126-S01"
        if round_number == 5:
            if not (directory / "models_frozen.json").is_file():
                raise RuntimeError("Cannot start independently acquired round without frozen models")
            label = "Z-AF-20261126-S02"
        banner = f"Round {round_number}, batch {batch_number}; {85-pending}/85 accepted; field session {label}"
        print(banner, flush=True)
        log.write(banner+"\n")
        capture_batch(round_number, label, log)
        after, _, _ = next_batch()
        if after >= pending:
            raise RuntimeError("Batch did not advance the accepted-pair ledger")
        print(f"Verified {85-after}/85 accepted pairs", flush=True)
