"""Offline checks. Synthetic records never enter the official acceptance ledger."""
from pathlib import Path
from tempfile import TemporaryDirectory
import json
from unittest.mock import patch

from plan import HERE,DEFAULTS,plan,preview
from evaluate import decision,e_score,reversal
from replay import capture,grade,np,pd
import controller


def synthetic(folder,slot):
    prefix=folder/slot["id"]
    stage=dict(schema="closeout_watch_v1",slot_id=slot["id"],outcome="complete",
               watch_aligned_contiguous=1,core_period_ns=50000,start_pulse=slot["positions"][0])
    tick=2**32-1000
    def event(key,i):
        stage[key+"_tick"]=(int(i)+tick)%2**32
    p,q=slot["positions"]
    event("setup_hold_begin",0)
    event("setup_hold_end",20000)
    event("leg1_before",20100)
    event("leg1_after",20101)
    peak=slot["speed"][0]
    acceleration=slot["acc"][0]
    ramp=peak/acceleration
    travel=abs(q-p)/1000
    total=travel/peak+ramp
    n=int(np.ceil(total/.00005))
    event("leg1_settled",20101+n)
    event("leg1_hold_begin",20101+n)
    m=int(slot["hold_ms"][0]*20)
    event("leg1_hold_end",20101+n+m)
    v=slot["speed"][0]
    a=slot["acc"][0]
    stage.update(leg1_speed_readback_pulse_ms=v,leg1_final_acc=a/1000,leg1_final_dec=a/1000,
                 leg1_target_readback_pulse=q)
    time=np.minimum(np.arange(n+1)*.00005,total)
    dist=np.where(time<ramp,.5*acceleration*time**2,np.where(time<total-ramp,peak*(time-ramp/2),travel-.5*acceleration*(total-time)**2))
    position=p+np.sign(q-p)*np.round(dist*1000)
    x=np.r_[np.full(20101,p),position,np.full(m+1,q)]
    i=np.arange(len(x))
    raw=pd.DataFrame(dict(frame=i,card_tick_raw=(i+tick)%2**32,encoder_position_raw=x,current_feedback_raw=550.))
    raw.to_csv(prefix.with_suffix(".csv"),index=False)
    prefix.with_suffix(".txt").write_text("".join(f"{k}={z}\n" for k,z in stage.items()))
    h=pd.DataFrame(dict(host_begin_us=[1,2,3],status=512,**{f"rc_{j}":0 for j in range(9)}))
    h.to_csv(prefix.with_suffix(".sdk.csv"),index=False)
    return raw,prefix


def reject(fn):
    try:
        fn()
    except (ValueError,OSError):
        return
    raise AssertionError("Invalid input accepted")


def main():
    r={}
    for kind in ("supplement","E"):
        p=plan(kind)
        assert len(p)==(24 if kind=="E" else 18) and len({t["id"] for t in p})==len(p)
        a=pd.DataFrame(preview(kind,dict(DEFAULTS,response=0),16000))
        assert a.geometry_ok.all()
        a.to_csv(HERE/f"{kind}_preview.csv",index=False,encoding="utf-8-sig")
        r[kind]=dict(slots=len(p),legs=len(a),min_nominal_cruise_mm=a.nominal_cruise_mm.min(),
                     min_margin_pulse=a.measurement_margin_pulse.min(),
                     blocked=list(a.loc[a.continuous_blocked,"id"]))
    assert len(r["E"]["blocked"])==6 and len(r["supplement"]["blocked"])==2
    for index in (1,2):
        for template in ("E1","E2","E3","E4"):
            subset=[t for t in plan("E") if t["family"]==template and t["session"]==index]
            assert [t["mirror"] for t in subset]==([False,True,False] if index==1 else [True,False,True])
    assert [grade("loading",x,y) for x,y in ((10,20),(10.001,20),(15,30),(15,30.001))]==["A","B","B","C"]
    assert grade("hold",8,16)=="A" and grade("hold",12,25)=="B" and grade("hold",12,25.1)=="C"
    counts={"cell":{"T0":4,"T1":2}}
    error={"slice":{"T0":dict(rows=10,rmse=5,p95=10),"T1":dict(rows=10,rmse=6,p95=12)}}
    delays={"SB-1":{"T0":5,"T1":15},"SB-2":{"T0":None,"T1":10}}
    assert decision(counts,delays,error,True)["choice"]=="T1"
    assert decision(counts,delays,error,False)["choice"] is None
    assert decision({"cell":{"T0":0,"T1":0}},delays,error,True)["choice"] is None
    assert decision({"cell":{"T0":0,"T1":0}},{"SB-1":{"T0":0},"SB-2":{"T0":5}},error,True)["choice"]=="T0"
    with TemporaryDirectory() as temp:
        folder=Path(temp)
        slot=next(x for x in plan("E") if x["family"]=="E1")
        raw,prefix=synthetic(folder,slot)
        clean,legs=capture(prefix,slot)
        for name in ("T0","T1"):
            result,_,table,waves,_,_,_=e_score([(slot,"synthetic",clean,legs)],name)
            assert result["scored_candidate"]==name and table.expected.max()==len(waves)
            assert set(table.support)=={"strict","extra"}
        bad=raw.drop(index=21000)
        bad.to_csv(prefix.with_suffix(".csv"),index=False)
        reject(lambda:capture(prefix,slot))
        assert not (folder/"score_started.json").exists()
    rv=np.r_[np.full(10,1000),0.,np.full(12,-1500)]
    ref=pd.DataFrame(dict(ref_v=rv))
    t=pd.DataFrame(dict(frame=np.arange(len(rv))*100,time_s=np.arange(len(rv))*.005,
                        T0_v=rv,T1_v=np.r_[rv[0],rv[:-1]]))
    events=reversal(t,ref,"synthetic",dict(direction=-1,settled=2300))
    assert all(e["hit"] for e in events) and events[1]["delay_ms"]-events[0]["delay_ms"]==5
    with TemporaryDirectory() as temp:
        path=Path(temp)
        # E must fail before accessing capture or issuing any command.
        with patch.object(controller,"directory",return_value=path):
            reject(lambda:controller.batch(path,{"kind":"E"}))
        with patch("builtins.input",return_value="SB-1"):
            reject(lambda:controller.batch(path,{"kind":"supplement","trials":plan("supplement")}))
    r["checks"]=["mirror schedule","grade boundary","50 us tick wrap","drop frame rejected",
                 "T0/T1 numeric scoring","reversal signed delay","release fail closed","engineering selection boundaries"]
    (HERE/"offline_verification.json").write_text(json.dumps(r,indent=2),encoding="utf-8")
    print(json.dumps(r,indent=2))


if __name__=="__main__":
    main()
