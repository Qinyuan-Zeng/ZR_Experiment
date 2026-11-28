"""Decode one complete Watch trace and score locked predictions."""
import math
from frozen_replay import c, predict
from diagnostics import inspect

np,pd=c.np,c.pd
TICK=.00005
WINDOWS=(0,.2,1,2,4,6)


def require(result, message):
    if not result:
        raise ValueError(message)


def capture(path, slot):
    w=pd.read_csv(path.with_suffix(".csv"))
    m=dict(line.split("=",1) for line in path.with_suffix(".txt").read_text().splitlines() if "=" in line)
    require(m.get("schema")=="closeout_watch_v1" and m.get("slot_id")==slot["id"] and
            m.get("outcome")=="complete" and m.get("watch_aligned_contiguous")=="1" and
            int(m["core_period_ns"])==50000 and int(m["start_pulse"])==slot["positions"][0],
            "Incomplete native acquisition")
    ticks=w.card_tick_raw.to_numpy(dtype=np.int64)
    require(len(ticks)>0 and np.array_equal((ticks-ticks[0])%2**32,np.arange(len(w))) and
            np.array_equal(w.frame,np.arange(len(w))) and np.isfinite(w.to_numpy(float)).all(),
            "Watch gap, duplication or invalid signal")
    x=w.encoder_position_raw.to_numpy()
    require(x.min()>=min(slot["positions"])-10 and x.max()<=max(slot["positions"])+10,
            "Watch position left scheduled stroke")
    host=pd.read_csv(path.with_suffix(".sdk.csv"))
    rc=[k for k in host if k.startswith("rc_")]
    status=host.status.to_numpy(dtype=np.int64)
    require(len(rc)==9 and host[rc].eq(0).all(axis=None) and host.host_begin_us.diff().dropna().gt(0).all()
            and np.all((status & ((1<<1)|(1<<5)|(1<<6)))==0) and np.all((status&(1<<9))!=0),
            "SDK status or timestamp failed")

    def when(key):
        i=(int(m[key+"_tick"])-int(ticks[0]))%2**32
        require(i<len(ticks),f"Event outside Watch span: {key}")
        return i

    def holds(begin,end,point,ms):
        v=x[begin:end+1]
        require(len(v)>0 and (end-begin+1)*TICK*1000>=ms and
                np.abs(v-point).max()<=10 and np.ptp(v)<=10,
                "Continuous HOLD or endpoint failed")

    pre=when("setup_hold_begin")
    end=when("setup_hold_end")
    holds(pre,end,slot["positions"][0],1000)
    legs=[]
    for i,(p,q,v,acc,dec,ms) in enumerate(zip(slot["positions"],slot["positions"][1:],slot["speed"],
                 slot["acc"],slot["dec"],slot["hold_ms"]),1):
        key=f"leg{i}"
        before,after,settled=[when(key+"_"+event) for event in ("before","after","settled")]
        require(end<=before<=after<settled,"Events out of order")
        require(int(m[key+"_target_readback_pulse"])==q and abs(float(m[key+"_speed_readback_pulse_ms"])-v)<=.01 and
                0.99*acc-6.104<=float(m[key+"_final_acc"])*1000<=1.01*acc and
                0.99*dec-6.104<=float(m[key+"_final_dec"])*1000<=1.01*dec,
                "Readback does not match frozen route")
        segment=x[after:settled+1]
        lag=100
        sign=np.sign(q-p)
        require(len(segment)>lag and abs(segment[0]-p)<=10 and abs(segment[-1]-q)<=10,
                "Motion start or arrival outside tolerance")
        speed=sign*(segment[lag:]-segment[:-lag])/5
        require(np.abs(speed).max()<=50 and np.quantile(speed,.95)>=.9*v,
                "Measured speed outside acceptance")
        if ms:
            start,end=[when(key+"_hold_"+event) for event in ("begin","end")]
            require(settled<=start<=end and np.abs(x[settled:end+1]-q).max()<=10,
                    "HOLD lost arrival position")
            holds(start,end,q,ms)
        else:
            require(m.get("planner_continuous_verified")=="1","Unverified continuous planner sequence")
            start=end=settled
        legs.append(dict(index=i,start=after,pre_command=before,settled=settled,hold_start=start,hold_end=end,
                         direction=int(sign),peak=v,acc=acc,dec=dec,target=q,name=f"leg{i}"))
    return w,legs


def grid_trace(w,slot,session,legs,first_frame=0,last_frame=None,trace=None):
    """Apply the V1 event/mask conventions before calling both frozen V3 paths."""
    if last_frame is None:
        last_frame=legs[-1]["hold_end"]
    k=np.arange(first_frame,last_frame+1,100)
    x_native=w.encoder_position_raw.to_numpy()
    tick=w.card_tick_raw.to_numpy()
    x=x_native[k]
    y=w.current_feedback_raw.to_numpy()[k]
    v,a=c.prep.derivative(x,5)
    va,aa=c.prep.derivative(x,7)
    hv,hs,hm,ts=c.prep.history_features(x,v)
    phase=np.full(len(k),"pre_hold",dtype="U12")
    segment=np.full(len(k),"pre",dtype="U12")
    known=k>=legs[0]["start"]
    moving=np.zeros(len(k),bool)
    stationary=moving.copy()
    short=moving.copy()
    transition=moving.copy()
    age=np.full(len(k),np.nan)
    direction=np.zeros(len(k),int)
    speed=np.zeros(len(k),float)
    phase[known]="transition"
    for j,leg in enumerate(legs):
        move=(k>=leg["start"])&(k<leg["settled"])
        hold=(k>=leg["settled"])&(k<=leg["hold_end"]) if leg["hold_end"]>leg["settled"] else moving&False
        st=np.select([leg["direction"]*a>=30000,leg["direction"]*a<=-30000],["accelerate","brake"],default="cruise")
        st=np.where(np.abs(v)<2000,"near_zero",st)
        phase[move]=st[move]
        phase[move & (k-leg["start"]<400)]="startup"
        phase[hold]="hold"
        segment[move]=leg["name"]
        segment[hold]="post"
        age[hold]=(k[hold]-leg["settled"])*TICK
        direction[move|hold]=leg["direction"]
        speed[move|hold]=leg["peak"]
        short |= move & (np.where(st=="brake",leg["peak"]/leg["dec"],leg["peak"]/leg["acc"])<=.020)&(st!="cruise")
        moving |= move
        stationary |= hold & (np.abs(x-leg["target"])<=10)&(np.abs(v)<=200)&(np.abs(va)<=200)
        if j==0 or legs[j-1]["direction"]!=leg["direction"] or legs[j-1]["hold_end"]>legs[j-1]["settled"]:
            transition |= np.abs(k-leg["start"])<=2000
        if j and legs[j-1]["direction"]!=leg["direction"]:
            transition |= np.abs(k-legs[j-1]["settled"])<=2000
    ip=c.read(c.prep.POSITION)
    alpha=c.read(c.prep.ALPHA)["alpha"]
    f1=c.read(c.prep.FRICTION)["models"]["F1"]
    ip_raw=np.where((x>=ip["range_pulse"][0])&(x<=ip["range_pulse"][1]),
                    np.interp(x,ip["node_pulse"],ip["current_raw"]),np.nan)
    fx=(x>=10000)&(x<=22000)
    ia=np.where(stationary,0,alpha*a)
    def sliding(vel):
        f=np.full(len(x),np.nan)
        f[stationary]=0
        for sign in (-1,1):
            v=vel*sign
            ok=moving&fx&(v>=2000)&(v<=40000)
            f[ok]=f1[str(sign)][0]+f1[str(sign)][1]*v[ok]/1000
        return f
    f=sliding(v)
    both=(np.abs(a-aa)<=np.maximum(30000,.2*np.maximum(np.abs(a),np.abs(aa))))&(np.abs(f-sliding(va))<5)
    alpha_ok=(np.abs(a)<30000)|((np.abs(a)>=390000)&(np.abs(a)<=610000))
    cohort="D" if slot["family"]=="H" else "B1" if slot["family"]=="SA" else "C"
    m1=ip_raw+ia+f
    strict=known&np.isfinite(m1)&(stationary|(moving&both&alpha_ok&~short&(cohort!="D")))
    t=pd.DataFrame(dict(unit=slot["id"],trace=trace or slot["id"],family=slot["family"],cohort=cohort,session=session,
        frame=k,card_tick_raw=tick[k],time_s=k*TICK,current_raw=y,phase=phase,segment=segment,hold_age_s=age,
        transition_100ms=transition,direction=direction,speed=speed,x_pulse=x,v_pulse_s=v,a_pulse_s2=a,
        arrival_memory=hm,v_history=hv,still_history=hs,time_since_stop=ts,ip_raw=ip_raw,ia_raw=ia,if_raw=f,m1_raw=m1,
        hold_zero=stationary,history_known=known,short_ramp=short,f1_position_valid=fx,candidate=strict,
        label_raw=np.where(strict,y-m1,np.nan)))
    features=c.v2.features(x,v,a,ts)
    t[["arrival_speed","travel"]]=features[:,1:3]
    ref=pd.DataFrame(dict(trace=t.trace,frame=k,
        ref_v=(x_native[np.minimum(k+100,len(w)-1)]-x_native[np.maximum(k-100,0)])/.010,
        ref_a=(x_native[np.minimum(k+200,len(w)-1)]-2*x+x_native[np.maximum(k-200,0)])/.010**2,
        dx_back=x-x_native[np.maximum(k-100,0)],dx_ahead=x_native[np.minimum(k+100,len(w)-1)]-x))
    ref.loc[(k<100)|(k+100>=len(w)),"ref_v"]=np.nan
    ref.loc[(k<200)|(k+200>=len(w)),"ref_a"]=np.nan
    return t,ref


def candidate_tables(t,names):
    out=t.copy()
    own={}
    for name in names:
        shaped,y,valid=predict(t,name)
        out[name]=y
        own[name]=valid
        out[name+"_strict"]=t.candidate.to_numpy()&valid
        out[name+"_native"]=valid
        out[name+"_extra"]=~t.candidate.to_numpy()&valid
        out[name+"_v"]=shaped.v_pulse_s
        out[name+"_a"]=shaped.a_pulse_s2
        out[name+"_lost"]=t.candidate.to_numpy()&~valid
    out["common"]=np.logical_and.reduce([out[name+"_strict"].to_numpy() for name in names])
    return out


def phases(t):
    yield "all",np.ones(len(t),bool)
    yield "loading",t.phase.isin(c.LOADING).to_numpy()
    for name in (*c.LOADING,"hold","startup","near_zero"):
        yield name,t.phase.eq(name).to_numpy()
    yield "switch_100ms",t.transition_100ms.to_numpy()
    for lo,hi in zip(WINDOWS[:-1],WINDOWS[1:]):
        yield f"hold_{lo:g}-{hi:g}", t.hold_age_s.ge(lo).to_numpy() & t.hold_age_s.lt(hi).to_numpy()
    yield "hold_2-6",t.hold_age_s.ge(2).to_numpy() & t.hold_age_s.lt(6).to_numpy()


def metric(t,name,mask):
    s=t.loc[mask]
    err=s.current_raw.to_numpy()-s[name].to_numpy()
    r=c.base.evaluate(s,err)
    if len(err):
        i=int(np.argmax(np.abs(err)))
        r.update(max_abs_raw=float(abs(err[i])),max_signed_raw=float(err[i]),max_signed_gf=float(err[i]*.59558),max_eq_gf=float(abs(err[i])*.59558),
                 peak_slot=s.unit.iloc[i],peak_trace=s.trace.iloc[i],peak_s=float(s.time_s.iloc[i]),
                 peak_tick=int(s.card_tick_raw.iloc[i]),peak_segment=s.segment.iloc[i])
    return r


def grade(stage,rmse,p95):
    if not math.isfinite(rmse) or not math.isfinite(p95):
        return "insufficient"
    a,b=(10,20),(15,30) if stage=="loading" else (12,25)
    if stage=="hold":
        a=(8,16)
    if rmse<=a[0] and p95<=a[1]:
        return "A"
    return "B" if rmse<=b[0] and p95<=b[1] else "C"
