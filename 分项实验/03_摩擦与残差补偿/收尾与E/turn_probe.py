"""Offline preview plus separately authorized PVT engineering probes."""
import argparse
from pathlib import Path
from uuid import uuid4
from tempfile import TemporaryDirectory
import io
import subprocess

from controller import c, identifier, locked, number, read_state, require, save, sha
from plan import HERE,ROOT,DEFAULTS,plan

np,pd=c.np,c.pd
EXE=ROOT/"bin/x64/Release/turn_probe/ZRFrictionCurrent.exe"
SOURCE=ROOT/"src/turn_probe.cpp"
PROBE=HERE/"turn_validation"
ROUTES=("SB-1","SB-2","E2-original","E2-mirror")


def route(name):
    if name.startswith("SB"):
        return next(t for t in plan("supplement") if t["id"]==name)
    return next(t for t in plan("E") if t["family"]=="E2" and t["mirror"]==(name=="E2-mirror"))


def planned(name):
    p=route(name)
    s=subprocess.check_output([str(EXE),"--plan",name],text=True)
    k=pd.read_csv(io.StringIO(s)).to_numpy()
    require(k.shape==(7,3) and np.isfinite(k).all(),"Invalid native PVT table")
    require(np.all(np.diff(k[:,0])>0) and k[0,0]==0,"PVT time must increase from zero")
    np.testing.assert_array_equal(k[[0,3,6],1],p["positions"])
    np.testing.assert_array_equal(k[[0,3,6],2],0)
    for i in range(2):
        j=i*3
        sign=np.sign(p["positions"][i+1]-p["positions"][i])
        peak=sign*p["speed"][i]
        np.testing.assert_allclose(k[j+1:j+3,2],peak)
        np.testing.assert_allclose(np.diff(k[j:j+4,2]) / np.diff(k[j:j+4,0])*1000,
                                   [sign*p["acc"][i],0,-sign*p["dec"][i]])
    # With dx = dt*(v0+v1)/2, the Hermite coefficients reduce exactly to
    # constant-acceleration sections; there is no flat segment at the turn.
    np.testing.assert_allclose(np.diff(k[:,1]), np.diff(k[:,0])*(k[1:,2]+k[:-1,2])/2,atol=1e-8)
    return k


def check_capture(attempt,name):
    p=route(name)
    k=planned(name)
    m=dict(line.split("=",1) for line in (attempt/"native.txt").read_text().splitlines() if "=" in line)
    require(m.get("route")==name and m.get("outcome")=="complete" and
            m.get("watch_contiguous")=="1" and m.get("trap_restored")=="1",
            "Native probe did not finish and restore")
    np.testing.assert_allclose([[float(x) for x in m[f"knot_{i}"].split(",")] for i in range(7)],k)
    w=pd.read_csv(attempt/"watch.csv")
    tick=w.card_tick_raw.to_numpy(dtype=np.int64)
    require(len(tick)>201 and np.isfinite(w.to_numpy(dtype=float)).all() and
            np.array_equal((tick-tick[0])%2**32,np.arange(len(tick))) and
            np.array_equal(w.frame,np.arange(len(tick))),"Incomplete native Watch")
    def when(event):
        i=(int(m[event+"_tick"])-int(tick[0]))%2**32
        require(i<len(tick),"Event outside Watch")
        return i
    pre0,pre1,before,after,arrive,post0,post1=(when(e) for e in
        ("initial_hold_begin","initial_hold_end","start_before","start_after","arrival","terminal_hold_begin","terminal_hold_end"))
    require(pre0<pre1<=before<=after<arrive<=post0<post1,"Probe event order invalid")
    x=w.encoder_position_raw.to_numpy()
    require(x.min()>=min(p["positions"])-10 and x.max()<=max(p["positions"])+10,"Feedback left scheduled stroke")
    for begin,end,point,ms in ((pre0,pre1,p["positions"][0],1000),(post0,post1,p["positions"][-1],p["hold_ms"][-1])):
        z=x[begin:end+1]
        require(len(z)>=ms*20 and np.max(np.abs(z-point))<=10 and np.ptp(z)<=10,"Hold stability or duration failed")

    idx=np.arange(after+100,arrive-100,100)
    vel=(x[idx+100]-x[idx-100])/.010  # pulse/s; two-sided offline reference only
    sign=np.sign(p["positions"][1]-p["positions"][0])
    expected=after+round(k[3,0]*20)
    region=idx[np.abs(idx-expected)<=2000]
    require(len(region)>0,"Apex evidence unavailable")
    apex=int(region[np.argmax(sign*x[region])])
    require(abs(x[apex]-p["positions"][1])<=10,"No validated apex")
    entry=(sign*vel>=600)&(idx<apex)
    exit_=(-sign*vel>=600)&(idx>apex)
    require(np.any(entry[1:]&entry[:-1]) and np.any(exit_[1:]&exit_[:-1]),"No sustained physical reversal")
    first_out=idx[np.flatnonzero(exit_[:-1]&exit_[1:])[0]]
    last_in=idx[np.flatnonzero(entry[:-1]&entry[1:])[-1]+1]
    speeds=[float(np.quantile(sign*vel[idx<apex],.95)/1000),float(np.quantile(-sign*vel[idx>apex],.95)/1000)]
    require(all(m>=.9*v for m,v in zip(speeds,p["speed"])) and np.abs(vel).max()<=50000,
            "Measured per-leg speed envelope failed")
    sdk=pd.read_csv(attempt/"sdk_velocity.csv")
    valid=sdk.loc[sdk.rc_velocity.eq(0)]
    require(len(valid)>2 and valid.host_begin_us.diff().dropna().gt(0).all(),"Missing SDK velocity timing")
    wp=pd.read_csv(attempt/"watch_batches.csv")
    require(len(wp)==3 and wp.rc.eq(0).all() and wp["count"].eq(len(w)).all(),"Watch readback columns disagree")
    return dict(route=name,frames=len(w),apex_frame=apex,apex_pulse=int(x[apex]),
                planned_turn_ms=float(k[3,0]),observed_apex_ms=float((apex-after)*.05),
                reference_bracket_ms=float((first_out-last_in)*.05),
                sdk_velocity_good_rows=len(valid),sdk_velocity_unique_values=int(valid.sdk_velocity_pulse_ms.nunique()),
                sdk_velocity_clock_distinct=int(valid.velocity_card_ms.nunique()),
                sdk_host_poll_p95_ms=float(valid.host_begin_us.diff().dropna().quantile(.95)/1000),
                achieved_p95_speed_mm_s=speeds,planner_restored=True,state="engineering_capture_checked")


def hashes():
    return {p.name:sha(p) for p in (EXE,EXE.with_name("gts.dll"),SOURCE,Path(__file__))}


def freeze(name):
    if name.startswith("E2"):
        for sb in ROUTES[:2]:
            p=PROBE/sb/"review.json"
            require(p.is_file(),"Both S-B probes must be reviewed first")
            reviewed=c.read(p)
            attempt=p.parent/"attempts"/identifier(reviewed["attempt"])
            a=c.read(attempt/"analysis.json")
            require(reviewed["code"]==hashes() and sha(attempt/"analysis.json")==reviewed["analysis_sha"]
                    and a["route"]==sb and all(sha(attempt/key)==h for key,h in a["inputs"].items()),
                    "S-B review evidence changed")
    path=PROBE/name
    require(not path.exists(),"Probe protocol exists")
    planned(name)
    l={k:number(f"Site {k}",v) for k,v in DEFAULTS.items()}
    l["response"]=number("Site-confirmed response time (ms)")
    table=number("Site-confirmed free PVT table ID (1-32)")
    require(1<=table<=32 and table.is_integer(),"Invalid table ID")
    coordinates=route(name)["positions"]
    need=l["reserve"]+l["follow"]+10+50*l["response"]+50**2/(2*l["stop"]) if l["stop"]>0 else np.inf
    require(0<=l["lo"]<=37000 and 0<=l["hi"]<=37000 and l["reserve"]>=1 and
            1<=l["follow"]<=500 and 0<=l["response"]<=1000 and
            min(min(coordinates)-l["lo"],l["hi"]-max(coordinates))>=need,"PVT geometry fails")
    name_id=identifier("Z-TURN-PROBE-"+name)
    path.mkdir(parents=True)
    note=dict(id=name_id,route=name,table=int(table),limits=l,code=hashes())
    save(path/"protocol.json",note)
    (path/"protocol.sha256").write_text(sha(path/"protocol.json")+"\n",encoding="ascii")
    print(name_id,"sealed offline; no motion")


def approved(path):
    protocol=path/"protocol.json"
    require(sha(protocol)==(path/"protocol.sha256").read_text().strip(),"Probe checksum mismatch")
    p=c.read(protocol)
    require(p["code"]==hashes(),"Probe code changed")
    return p


def run(path,p):
    require(p["route"]==path.name,"Route mismatch")
    with locked(ROOT/"manifest/formal_a","hardware.lock"):
        require(input(f"Confirm approved scope, reserved PVT table and field conditions. Type {p['id']}: ").strip()==p["id"],
                "Site authorization missing")
        a=path/"attempts"/uuid4().hex
        a.mkdir(parents=True)
        (a/"limits.txt").write_text(" ".join(str(p["limits"][k]) for k in ("lo","hi","reserve","follow","stop","response"))
                                    +f" {p['table']}\n",encoding="ascii")
        def snapshot(label):
            r=subprocess.run([str(EXE),"--state"],capture_output=True,text=True,encoding="ascii",errors="replace")
            file=a/(label+".txt")
            file.write_text(r.stdout+r.stderr,encoding="utf-8")
            require(r.returncode==0,f"Independent state check failed: {file}")
            return read_state(file,p["limits"])
        position=snapshot("pre")
        require(abs(position-route(p["route"])["positions"][0])<=2,"Prepare the exact first knot before starting")
        rc=1
        try:
            rc=subprocess.call([str(EXE),"--capture",p["route"],str(a)])
        finally:
            save(a/"exit.json",dict(returncode=rc))
            snapshot("post")
        require(rc==0,"Engineering capture incomplete")
        result=check_capture(a,p["route"])
        files=("native.txt","watch.csv","sdk_velocity.csv","watch_batches.csv","pre.txt","post.txt","exit.json","limits.txt")
        save(a/"analysis.json",dict(result,inputs={name:sha(a/name) for name in files}))
        print(a,result)


def review(path,p,attempt):
    folder=path/"attempts"/identifier(attempt)
    analysis=folder/"analysis.json"
    a=c.read(analysis)
    require(a["route"]==p["route"] and all(sha(folder/name)==h for name,h in a["inputs"].items()),"Capture evidence changed")
    require(not (path/"review.json").exists(),"Review already exists")
    reviewer=identifier(input("Reviewer confirming waveform, no inserted dwell, two attained speeds and stop behavior: ").strip())
    require(input(f"Confirm {p['id']} engineering review: ").strip()==p["id"],"Review not signed")
    save(path/"review.json",dict(attempt=attempt,analysis_sha=sha(analysis),code=hashes(),reviewer=reviewer))


def synthetic_capture(name, table):
    p=route(name)
    with TemporaryDirectory() as tmp:
        dst=Path(tmp)
        start,tick=20100,2**32-60
        n=int(np.ceil(table[-1,0]/.05))
        q=np.minimum(np.arange(n+1)*.05,table[-1,0])
        i=np.searchsorted(table[:,0],q,side="right").clip(1,6)-1
        elapsed=q-table[i,0]
        a=np.diff(table[:,2])/np.diff(table[:,0])
        x=np.round(table[i,1]+table[i,2]*elapsed+.5*a[i]*elapsed**2)
        arrive=start+n+200
        post=arrive+p["hold_ms"][-1]*20
        x=np.r_[np.full(start,p["positions"][0]),x,np.full(post+1-start-len(x),p["positions"][-1])]
        events=dict(initial_hold_begin=0,initial_hold_end=20000,start_before=start-1,start_after=start,
                    arrival=arrive,terminal_hold_begin=arrive,terminal_hold_end=post)
        d=dict(route=name,outcome="complete",watch_contiguous=1,trap_restored=1)
        d.update({f"{key}_tick":(tick+v)%2**32 for key,v in events.items()})
        d.update({f"knot_{i}":",".join(map(str,k)) for i,k in enumerate(table)})
        (dst/"native.txt").write_text("".join(f"{key}={v}\n" for key,v in d.items()))
        frames=np.arange(len(x))
        watch=pd.DataFrame(dict(frame=frames,card_tick_raw=(tick+frames)%2**32,
                                encoder_position_raw=x,current_feedback_raw=500))
        watch.to_csv(dst/"watch.csv",index=False)
        pd.DataFrame(dict(column=[1,2,3],rc=0,count=len(x))).to_csv(dst/"watch_batches.csv",index=False)
        pd.DataFrame(dict(host_begin_us=[1,2,3],sdk_velocity_pulse_ms=[0,1,0],
                          velocity_card_ms=[1,2,3],rc_velocity=0)).to_csv(dst/"sdk_velocity.csv",index=False)
        assert check_capture(dst,name)["state"]=="engineering_capture_checked"
        watch.drop(index=start+100).to_csv(dst/"watch.csv",index=False)
        try:
            check_capture(dst,name)
        except ValueError as err:
            assert "Incomplete native Watch" in str(err),err
        else:
            raise AssertionError("Dropped frame unexpectedly passed")


def selftest(verify_capture):
    rows=[]
    for name in ROUTES:
        table=planned(name)
        if verify_capture:
            synthetic_capture(name,table)
        rows.extend(dict(route=name,knot=i,time_ms=t,position_pulse=x,velocity_pulse_ms=v)
                    for i,(t,x,v) in enumerate(table))
    pd.DataFrame(rows).to_csv(HERE/"turn_pvt_points.csv",index=False,encoding="utf-8-sig")
    print("Four native knot tables verified, one zero-speed apex each, no duplicate timestamp.")
    if verify_capture:
        print("Four synthetic Watch traces passed; four missing-frame files rejected.")


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("operation",choices=["preview","selftest","freeze","capture","review"])
    p.add_argument("--route",choices=ROUTES)
    p.add_argument("--attempt")
    args=p.parse_args()
    try:
        if args.operation in ("preview","selftest"):
            selftest(args.operation=="selftest")
            if args.operation=="preview":
                for name in ROUTES:
                    print(name,planned(name),sep="\n")
        else:
            require(args.route,"Specify --route")
            path=PROBE/args.route
            if args.operation=="freeze":
                freeze(args.route)
            else:
                with locked(path):
                    f=approved(path)
                    if args.operation=="capture":
                        run(path,f)
                    else:
                        require(args.attempt,"Specify --attempt")
                        review(path,f,args.attempt)
    except (OSError,ValueError,KeyError,AssertionError,EOFError,KeyboardInterrupt) as e:
        p.exit(1,f"Turn probe: {e or 'cancelled or invalid input'}\n")


if __name__=="__main__":
    main()
