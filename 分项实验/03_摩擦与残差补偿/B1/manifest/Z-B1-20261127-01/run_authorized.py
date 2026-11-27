"""Monitor one authorized B1 scope and stop issuing requests when a batch fails."""
import os
from pathlib import Path
import subprocess
import sys

root=Path.cwd()/"分项实验/03_摩擦与残差补偿"
sys.path.insert(0,str(root/"scripts"))
import b1_run as b

protocol="Z-B1-20261127-01"
directory=b.MANIFEST/protocol
if sys.argv[1:] not in (["pilot"],["full"]):
    raise SystemExit("Specify the authorized scope as pilot or full")
pilot=sys.argv[1]=="pilot"
env=dict(os.environ,PYTHONIOENCODING="utf-8",PYTHONDONTWRITEBYTECODE="1")
cli=[sys.executable,"-u",str(root/"scripts/b1_run.py"),"--batch",protocol]
if pilot:
    cli.append("--pilot")

def run_one(label,log):
    responses=[
        ("Actual field session ID: ",label),
        ("Confirm field state, authorized scope and safeguards (including existing drive protection). Type READY: ","READY"),
        ("Type START to execute the displayed B1 batch: ","START")
    ]
    prechecked=False
    buffer=""
    with subprocess.Popen(cli,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,
                          text=True,encoding="utf-8",errors="replace",env=env,bufsize=1) as child:
        while char:=child.stdout.read(1):
            buffer+=char
            if responses and buffer.endswith(responses[0][0]):
                prompt,response=responses.pop(0)
                if response=="START" and not prechecked:
                    child.stdin.close()
                    raise RuntimeError("No motion: independent precheck evidence is missing")
                print(buffer+response,flush=True)
                log.write(buffer+response+"\n")
                child.stdin.write(response+"\n")
                child.stdin.flush()
                buffer=""
            elif char=="\n":
                log.write(buffer)
                if "Independent precheck and route clearance passed." in buffer:
                    prechecked=True
                if any(s in buffer for s in ("accepted","pending:","complete","Independent precheck","B1:","interrupted")):
                    print(buffer.strip(),flush=True)
                buffer=""
        if buffer:
            log.write(buffer+"\n")
            print(buffer,flush=True)
        code=child.wait()
    if code or responses:
        raise SystemExit(code or 1)

with b.locked(directory,"supervisor.lock"), (directory/"run_console.txt").open("a",encoding="utf-8",buffering=1) as log:
    frozen=b.verified(protocol,directory)
    while True:
        known=b.accepted(directory)
        pending=b.queue_for(frozen,known,pilot)
        if not pending:
            print(f"Authorized scope complete: {sum(s['id'] in known for s in frozen['slots'] if not pilot or s['pilot'])} pairs",flush=True)
            break
        first=pending[0]
        field="Z-B1-20261127-S01" if first["period"]=="S1" else "Z-B1-20261127-S02"
        message=f"{len(known)}/160 accepted; next {first['id']} in {field}"
        print(message,flush=True); log.write(message+"\n")
        run_one(field,log)
        added=len(b.accepted(directory))-len(known)
        if not 1<=added<=4:
            raise RuntimeError("Unexpected progress count; stop with records intact")
