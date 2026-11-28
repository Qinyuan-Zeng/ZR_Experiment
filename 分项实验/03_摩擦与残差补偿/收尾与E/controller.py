"""Separate acquisition scopes with hash-bound quality and one-shot scoring."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import subprocess
import sys
from uuid import uuid4

from plan import HERE,ROOT,DEFAULTS,plan,preview
from frozen_replay import V3,c,sha,weight
from replay import capture,require
from evaluate import select,e_score,pd

sys.path.insert(0,str(ROOT/"scripts"))
from a_formal import identifier,locked,save
from b1_run import read_state,number

PROTOCOL={"supplement":"Z-CLOSEOUT-S01","E":"Z-CLOSEOUT-E01"}
SPEC=ROOT/"第三阶段收尾与E独立验证方案.md"


def executable(kind):
    return ROOT/"bin/x64/Release"/("e_scan" if kind=="E" else "supplement_scan")/"ZRFrictionCurrent.exe"


def directory(kind):
    return HERE/kind/"manifest"/PROTOCOL[kind]


def protected():
    paths=[SPEC,HERE/"historical_reproduction.json",*HERE.glob("*.py"),
           c.prep.POSITION,c.prep.ALPHA,c.prep.FRICTION,ROOT/"B0/audit_provenance.json",
           c.DATA,c.V1/"prepare_joint.py",c.V1/"train_joint.py",c.V2/"compare.py",
           *(V3/name for name in ("protocol.json","state.py","common.py","diagnostics.py")),
           *(weight(name) for name in ("T0","T1")),
           *(ROOT/"src"/f"{name}.cpp" for name in ("closeout_capture","supplement_scan","e_scan")),
           *(ROOT.parent/"02_等效惯性电流辨识/include"/name for name in
             ("matched_motion.hpp","matched_batch.hpp","watch_capture.hpp")),
           *(executable(k) for k in PROTOCOL),*(executable(k).with_name("gts.dll") for k in PROTOCOL)]
    return {str(p):sha(p) for p in sorted(paths)}


def write_new(path,value):
    require(not path.exists(),f"Already sealed: {path}")
    save(path,value)


def verify(path):
    p=c.read(path/"protocol.json")
    require((path/"protocol.sha256").read_text().strip()==sha(path/"protocol.json"),"Protocol hash mismatch")
    require(p["protected"]==protected() and p["trials"]==plan(p["kind"]),"Protected files or point table changed")
    return p


def freeze():
    path=directory("supplement")
    require(not path.exists(),"Supplement already frozen")
    repro=c.read(HERE/"historical_reproduction.json")
    require(set(repro)=={"T0","T1"} and all(x["max_abs_raw"]<1e-7 for x in repro.values()),"Candidate parity missing")
    limits={key:number(f"Site protection {key}",value) for key,value in DEFAULTS.items()}
    limits["response"]=number("Site-confirmed response time (ms)")
    pos=number("Observed stationary start coordinate (pulse)")
    for kind in PROTOCOL:
        require(all(r["geometry_ok"] for r in preview(kind,limits,pos)),f"{kind} geometry not feasible")
    reviewer=identifier(input("Reviewer ID for approved engineering selection rules: ").strip())
    require(input(f"Type {PROTOCOL['supplement']} to sign the published rules and point tables: ").strip()==PROTOCOL["supplement"],
            "Protocol signature not issued")
    path.mkdir(parents=True)
    p=dict(kind="supplement",id=PROTOCOL["supplement"],trials=plan("supplement"),e_reserved=plan("E"),
           limits=limits,reviewer=reviewer,protected=protected())
    write_new(path/"protocol.json",p)
    (path/"protocol.sha256").write_text(sha(path/"protocol.json")+"\n",encoding="ascii")


def ledger(path,p,load_waveforms=False):
    known={}
    records=[]
    for a in sorted((path/"attempts").glob("*/accepted.json")):
        ev=c.read(a)
        slot=next(row for row in p["trials"] if row["id"]==ev["id"])
        require(ev["id"] not in known,"Duplicate accepted slot")
        for f,h in ev["inputs"].items():
            require(sha(f)==h,f"Accepted evidence changed: {f}")
        known[ev["id"]]=ev
        if load_waveforms:
            w,legs=capture(Path(ev["prefix"]),slot)
            records.append((slot,ev["session"],w,legs))
    return known,records


def state(exe,path,limits):
    r=subprocess.run([str(exe),"--state"],capture_output=True,text=True,encoding="ascii",errors="replace")
    path.write_text(r.stdout+r.stderr,encoding="utf-8")
    require(r.returncode==0,f"Independent axis check failed: {path}")
    return read_state(path,limits)


def verify_release(p):
    path=directory("supplement")
    selected=path/"selected.json"
    report=path/"selection.json"
    seal=c.read(selected)
    require(p["selection_sha256"]==sha(selected) and seal["report_sha256"]==sha(report) and
            p["candidate"]==seal["candidate"] and seal["candidate"]==c.read(report)["choice"] and
            p["candidate_sha256"]==sha(weight(p["candidate"])),"E release or selected candidate changed")


def batch(path,p):
    kind=p["kind"]
    if kind=="E":
        verify_release(p)
        require(not (path/"score_started.json").exists(),"E scoring has started")
    known,_=ledger(path,p)
    pending=[t for t in p["trials"] if t["id"] not in known]
    if not pending:
        print("Authorized plan has no pending slots")
        return
    print("Pending:",", ".join(t["id"] for t in pending))
    name=identifier(input("Authorized slot ID: ").strip())
    slot=next(t for t in pending if t["id"]==name)
    require(0 not in slot["hold_ms"],"Continuous turn backend is unverified for this slot")
    if kind=="E":
        # No formal E data can be acquired while any locked template lacks its approved backend.
        require(all(0 not in t["hold_ms"] for t in p["trials"]),
                "E blocked until validated buffered continuous E2 entry supports every template")
    session=identifier(input("Actual field session ID: ").strip())
    if kind=="E":
        planned_index={t["id"]:t["session"] for t in p["trials"]}
        require(all((planned_index[k]==slot["session"])==(v["session"]==session) for k,v in known.items()),
                "Session must match its own slots and differ from other session")
        if not any(planned_index[k]==slot["session"] for k in known):
            require(input("Confirm separately started or date-separated E session: ").strip()==session,
                    "Actual session independence not confirmed")
    require(input("Confirm this protocol, scope, field conditions and safeguards with READY: ").strip()=="READY",
            "现场尚未确认")
    a=path/"attempts"/uuid4().hex
    a.mkdir(parents=True)
    exe=executable(kind)
    limits=p["limits"]
    pos=state(exe,a/"pre.txt",limits)
    x=slot["positions"]
    budget=lambda cap: limits["reserve"]+limits["follow"]+10+cap*cap/(2*limits["stop"])+cap*limits["response"]
    for v,w,cap in [(pos,x[0],5),*((i,j,50) for i,j in zip(x,x[1:]))]:
        require(min(min(v,w)-limits["lo"],limits["hi"]-max(v,w))>=budget(cap),"Actual-position clearance failed")
    write_new(a/"request.json",dict(slot=slot,session=session,protocol_sha256=sha(path/"protocol.json")))
    values=[limits[k] for k in ("lo","hi","reserve","follow","stop","response")]
    args=[slot["id"],x[0],len(x)-1,*[k for leg in zip(x[1:],slot["speed"],slot["acc"],slot["dec"],slot["hold_ms"]) for k in leg]]
    (a/"request.txt").write_text(" ".join(map(str,[*values,1,*args]))+"\n",encoding="ascii")
    if kind=="E":
        (a/"release.receipt").write_text(sha(path/"protocol.json")+"\n",encoding="ascii")
    raw=HERE/kind/"data/raw"/p["id"]/a.name/slot["id"]
    rc=1
    try:
        print("Independent precheck passed",flush=True)
        rc=subprocess.call([str(exe),"--run-batch",p["id"],a.name])
    finally:
        write_new(a/"exit.json",dict(returncode=rc))
        state(exe,a/"post.txt",limits)
    require(rc==0,"Native acquisition incomplete; raw kept")
    capture(raw,slot)
    paths=[raw.with_suffix(ext) for ext in (".csv",".txt",".sdk.csv",".batches.csv")]
    paths.extend([raw.parent/"guard.csv",a/"pre.txt",a/"post.txt",a/"request.json",a/"request.txt",a/"exit.json",a/"native.txt"])
    write_new(a/"accepted.json",dict(id=name,session=session,prefix=str(raw),inputs={str(f):sha(f) for f in paths}))
    print(f"{len(known)+1}/{len(p['trials'])} accepted")


def perform_selection(path,p):
    require(not (path/"selection.json").exists(),"Selection already recorded")
    _,records=ledger(path,p,True)
    require(len(records)==18,"All 18 supplement slots must pass acquisition before selection")
    report,metrics,waves,events,timing=select(records)
    for name,frame in (("selection_metrics.csv",metrics),("selection_waves.csv.gz",waves),
                       ("selection_false_signs.csv",events),("selection_reversal_timing.csv",timing)):
        frame.to_csv(path/name,index=False)
    write_new(path/"selection.json",report)


def reserve_e(path,p):
    report=path/"selection.json"
    r=c.read(report)
    require(r["passes"] and r["choice"] in ("T0","T1"),"Engineering evidence insufficient")
    require(not directory("E").exists(),"E protocol already sealed")
    reviewer=identifier(input("Reviewer ID for severe-anomaly and allowed-use review: ").strip())
    purpose=input("Approved purpose and limitations: ").strip()
    require(purpose and input(f"Type {r['choice']} to sign unique candidate: ").strip()==r["choice"],
            "Candidate not signed")
    e_dir=directory("E")
    # Existing A/B1/C/D and supplement endpoint signatures from frozen plans.
    def points(root,key):
        return c.read(root/"protocol.json")[key]
    b=points(ROOT/"B1/manifest/Z-B1-20261127-02","slots")
    cc=points(ROOT/"C/manifest/Z-C-HISTORY-01","trials")
    dd=points(ROOT/"D/manifest/Z-D-HOLD-01","trials")
    existing={tuple(v["positions"]) for v in p["trials"]}
    existing|={(v["lo"],v["hi"]) for v in b}|{(v["hi"],v["lo"]) for v in b}
    existing|={(v["start"],16000,v["end"]) for v in cc}|{(v["start"],v["target"]) for v in dd}
    a_doc=c.read(ROOT/"manifest/formal_a/Z-AF-20261126-03/protocol.json")
    a_start,a_end=(a_doc["params"][k] for k in ("lo","hi"))
    existing|={(a_start,a_end),(a_end,a_start)}
    require(not any(tuple(v["positions"]) in existing for v in p["e_reserved"]),
            "Prespecified E conflicts with historic endpoint signature")
    selected=path/"selected.json"
    write_new(selected,dict(candidate=r["choice"],report_sha256=sha(report),reviewer=reviewer,limitations=purpose))
    e_dir.mkdir(parents=True)
    ep=dict(kind="E",id=PROTOCOL["E"],candidate=r["choice"],candidate_sha256=sha(weight(r["choice"])),
            selection_sha256=sha(selected),trials=p["e_reserved"],limits=p["limits"],protected=p["protected"])
    write_new(e_dir/"protocol.json",ep)
    (e_dir/"protocol.sha256").write_text(sha(e_dir/"protocol.json")+"\n",encoding="ascii")


def formal_score(path,p):
    verify_release(p)
    known,records=ledger(path,p,True)
    require(len(records)==24,"All 24 E acquisitions must pass frozen quality checks")
    sessions={r: {known[t["id"]]["session"] for t in p["trials"] if t["session"]==r} for r in (1,2)}
    require(all(len(v)==1 for v in sessions.values()) and not sessions[1]&sessions[2],
            "Independent session labels inconsistent")
    require(input("Confirm the two separately started/date-separated sessions and quality seal: ").strip()==p["id"],
            "E scoring not confirmed")
    write_new(path/"score_started.json",dict(candidate=p["candidate"],at_utc=datetime.now(timezone.utc).isoformat(),
                  accepted={key:val["inputs"] for key,val in known.items()}))
    result,ranked,metrics,waves,peaks,worst,switches=e_score(records,p["candidate"])
    for name,frame in (("grades.csv",ranked),("metrics.csv",metrics),("waveforms.csv.gz",waves),
                       ("peak_windows.csv",peaks),("worst_by_speed_direction.csv",worst),("switch_diagnostics.csv",switches)):
        frame.to_csv(path/name,index=False)
    write_new(path/"grade.json",result)


def cli(kind):
    parser=argparse.ArgumentParser(description=__doc__)
    choices=["preview","pending","batch"]+(["freeze","select","seal-e"] if kind=="supplement" else ["score"])
    parser.add_argument("operation",choices=choices)
    parser.add_argument("--position",default=16000,type=float)
    parser.add_argument("--response-ms",default=0,type=float)
    args=parser.parse_args()
    path=directory(kind)
    try:
        if args.operation=="preview":
            rows=preview(kind,dict(DEFAULTS,response=args.response_ms),args.position)
            print(pd.DataFrame(rows).to_string(index=False))
            return
        if args.operation=="freeze":
            freeze()
            return
        require(path.is_dir(),"No frozen protocol. Use preview first.")
        with locked(path):
            p=verify(path)
            if args.operation=="batch":
                with locked(ROOT/"manifest/formal_a","hardware.lock"):
                    batch(path,p)
            elif args.operation=="pending":
                known,_=ledger(path,p)
                print([t["id"] for t in p["trials"] if t["id"] not in known])
            elif args.operation=="select":
                perform_selection(path,p)
            elif args.operation=="seal-e":
                reserve_e(path,p)
            else:
                formal_score(path,p)
    except (OSError,ValueError,KeyError,AssertionError,StopIteration,EOFError,KeyboardInterrupt) as e:
        parser.exit(1,f"Closeout: {e or 'cancelled or invalid input'}\n")
