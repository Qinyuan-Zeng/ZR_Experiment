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
resolve_existing=len(sys.argv)==3 and sys.argv[1]=="resolve-recorded-post"
if sys.argv[1:] not in (["pilot"],["full"]) and not resolve_existing:
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


def same_stop(f,path,limits):
    pos=b.read_state(path,limits)
    later=b.fields(path)
    required={"enabled":"1","moving":"0","fault":"0","limit_plus":"0","limit_minus":"0","mode":"2","planner":"0"}
    if any(f.get(k)!=v or later.get(k)!=v for k,v in required.items()):
        raise ValueError("Read-only recheck does not match the confirmed safe axis state")
    if float(f["planned_pulse"])!=float(later["planned_pulse"]) or abs(float(f["position_pulse"])-pos)>10:
        raise ValueError("Position changed between checks")
    if abs(float(f["following_error_pulse"]))>limits["follow"] or float(f["planned_velocity_pulse_s"])!=0:
        raise ValueError("Failed snapshot also contains a motion or protection discrepancy")

def exact_velocity_spike(s):
    return s.get("ready")=="0" and s.get("reason")=="not_idle_or_not_aligned" and abs(float(s["velocity_pulse_s"]))==20000

def recheck_snapshot(attempt,first,l,label):
    for filename in (label+"_recheck.txt",label+"_recheck_2.txt"):
        p=attempt/filename
        if not p.exists():
            try:
                b.capture_state(p,l)
            except ValueError:
                if not p.is_file() or not exact_velocity_spike(b.fields(p)):
                    raise
        state=b.fields(p)
        if exact_velocity_spike(state):
            continue
        same_stop(first,p,l)
        return p
    raise ValueError("Two independent idle rechecks failed; inspect before issuing a new batch")

def recover_known(attempt,frozen):
    import csv
    from datetime import datetime, timezone
    from a0_replay import digest
    l=frozen["limits"]
    with b.locked(directory), b.locked(root/"manifest/formal_a","hardware.lock"):
        latest=max(b.attempts(directory),key=lambda a:(a/"request.json").stat().st_mtime)
        if latest!=attempt or (attempt/"recovery_resolution.json").exists():
            return False
        post=attempt/"post.txt"
        pre=attempt/"pre.txt"
        native=attempt/"native.txt"
        if not (attempt/"request.txt").exists() and not native.exists() and not post.exists() and pre.is_file():
            first=b.fields(pre)
            if not exact_velocity_spike(first):
                return False
            checked=recheck_snapshot(attempt,first,l,"pre")
            observed=pre
            kind="precheck_velocity_spike_before_opening_native_acquisition"
        elif post.is_file() and native.is_file() and (attempt/"exit.json").is_file():
            first=b.fields(post)
            meta=b.fields(native)
            exit_code=b.load(attempt/"exit.json")["returncode"]
            if exit_code==0 and meta.get("outcome")=="complete" and exact_velocity_spike(first):
                checked=recheck_snapshot(attempt,first,l,"post")
                if (attempt/"post_initial_failed.txt").exists():
                    return False
                observed=attempt/"post_initial_failed.txt"
                post.replace(observed)
                pending=attempt/"post.pending"
                pending.write_bytes(checked.read_bytes())
                pending.replace(post)
                kind="complete_batch_use_independent_post_recheck"
            elif exit_code==1 and meta.get("outcome")=="incomplete":
                folder=b.B1/"data/raw"/protocol/attempt.name
                failures=[p for p in folder.glob("*_leg*.txt") if b.fields(p).get("outcome")!="complete"]
                if len(failures)!=1 or b.fields(failures[0]).get("reason")!="B1 transfer initial state is not idle/aligned":
                    return False
                observed=failures[0].with_suffix(".sdk.csv")
                with observed.open(encoding="ascii",newline="") as stream:
                    entries=list(csv.DictReader(stream))
                if len(entries)!=1:
                    return False
                row=entries[0]
                pos=float(row["encoder_pulse"])
                plan=float(row["planner_pulse"])
                status=int(row["status"])
                if row["phase"]!="transfer_before" or abs(float(row["velocity_pulse_ms"]))!=20 or \
                   abs(pos-plan)>10 or float(row["planner_vel_pulse_ms"])!=0 or \
                   abs(float(row["following_error_pulse"]))>l["follow"] or status!=1073742336 or \
                   any(int(row[k])!=0 for k in row if k.startswith("rc_")):
                    return False
                later=b.fields(post)
                state=dict(later,position_pulse=str(pos),planned_pulse=str(plan),
                           following_error_pulse=row["following_error_pulse"])
                same_stop(state,post,l)
                checked=post
                kind="velocity_spike_before_next_transfer"
            else:
                return False
        else:
            return False
        b.save(attempt/"recovery_resolution.json",dict(
            reason=kind,original=str(observed),original_sha256=digest(observed),
            independent_passed_state=str(checked),independent_passed_state_sha256=digest(checked),
            at_utc=datetime.now(timezone.utc).isoformat()))
        b.replay(directory,frozen)
        print("Verified independent stop recheck:",kind,flush=True)
        return True

if resolve_existing:
    with b.locked(directory,"supervisor.lock"):
        frozen=b.verified(protocol,directory)
        attempt=directory/"attempts"/b.identifier(sys.argv[2])
        if not recover_known(attempt,frozen):
            raise SystemExit("Known post-batch condition was not confirmed")
    raise SystemExit(0)

with b.locked(directory,"supervisor.lock"), (directory/"run_console.txt").open("a",encoding="utf-8",buffering=1) as log:
    frozen=b.verified(protocol,directory)
    idle_retries={}
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
        before=set(b.attempts(directory))
        recovered=False
        try:
            run_one(field,log)
        except SystemExit:
            after=set(b.attempts(directory))-before
            idle_retries[first["id"]]=idle_retries.get(first["id"],0)+1
            if len(after)!=1 or idle_retries[first["id"]]>2 or not recover_known(after.pop(),frozen):
                raise
            recovered=True
        added=len(b.accepted(directory))-len(known)
        if not (0 if recovered else 1)<=added<=4:
            raise RuntimeError("Unexpected progress count; stop with records intact")
