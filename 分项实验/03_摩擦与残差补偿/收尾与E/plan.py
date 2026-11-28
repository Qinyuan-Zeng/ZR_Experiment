"""Approved closeout schedules and offline geometry."""
from pathlib import Path
import itertools
import math

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
DEFAULTS = dict(lo=0,hi=37000,reserve=4000,follow=500,stop=.99)
CAP = 50
TRANSFER_CAP = 5


def record(id_, family, positions, velocity, accel, holds, decel=None, **extra):
    return dict(id=id_, family=family, positions=list(positions), speed=list(velocity),
                acc=list(accel), dec=list(accel if decel is None else decel), hold_ms=list(holds), **extra)


def supplement():
    result = []
    for r, v, d in itertools.product((1,2),(30,40),(400,600)):
        p = (12000,20000) if (r+(v==40)+(d==600))%2 else (20000,12000)
        result.append(record(f"SA-R{r}-V{v}-D{d}","SA",(*p,p[0]),(v,v),(400,400),(1000,1000),(d,d),repeat=r))
    for i,p in enumerate(((15000,17000,15000),(17000,15000,17000)),1):
        result.append(record(f"SB-{i}","SB",p,(10,10),(400,400),(0,1000),repeat=1))
    for r,v,sign in itertools.product((1,2),(2,5),(1,-1)):
        p="P" if sign==1 else "N"
        result.append(record(f"H-R{r}-V{v}-{p}","H",(16000-4000*sign,16000),(v,),(400,),(6000,),repeat=r))
    return result


def independent():
    templates = [
        ((11000,19000),(18,),(500,),(6000,)),
        ((10500,21500,15500),(38,12),(600,400),(0,3000)),
        ((13000,18500,14500),(28,3),(500,400),(350,6000)),
        ((11500,16500,20500),(32,5),(400,500),(750,6000)),
    ]
    result = []
    for session,r in itertools.product((1,2),(1,2,3)):
        order = [(i+r+session-2)%4 for i in range(4)]
        mirrored = (r+session)%2==1
        for i in order:
            p,v,a,h=templates[i]
            if mirrored:
                p=tuple(32000-x for x in p)
            result.append(record(f"E{i+1}-S{session}-R{r}",f"E{i+1}",p,v,a,h,
                                 session=session,repeat=r,mirror=mirrored))
    return result


def plan(kind):
    return independent() if kind=="E" else supplement()


def preview(kind,limits,start):
    assert math.isfinite(start) and all(math.isfinite(x) for x in limits.values())
    assert 0<=limits["lo"]<start<limits["hi"]<=37000 and 1<=limits["follow"]<=500
    assert limits["reserve"]>=1 and limits["stop"]>0 and 0<=limits["response"]<=1000
    def margin(a,b,cap):
        need=limits["reserve"]+limits["follow"]+10+cap**2/(2*limits["stop"])+cap*limits["response"]
        return min(min(a,b)-limits["lo"],limits["hi"]-max(a,b))-need
    rows=[]
    for t in plan(kind):
        p=t["positions"]
        transfer=margin(start,p[0],TRANSFER_CAP)
        for i,(a,b,v,acc,dec,hold) in enumerate(zip(p,p[1:],t["speed"],t["acc"],t["dec"],t["hold_ms"]),1):
            ramp=v*v/2*(1/acc+1/dec)
            m=margin(a,b,CAP)
            rows.append(dict(id=t["id"],leg=i,start=a,end=b,peak_mm_s=v,acc_mm_s2=acc,dec_mm_s2=dec,
                nominal_cruise_mm=abs(b-a)/1000-ramp,nominal_brake_ms=1000*v/dec,
                hold_ms=hold,measurement_margin_pulse=m,transfer_margin_pulse=transfer,
                geometry_ok=abs(b-a)/1000>ramp and min(transfer,m)>=0,
                continuous_blocked=hold==0))
        start=p[-1]
    return rows
