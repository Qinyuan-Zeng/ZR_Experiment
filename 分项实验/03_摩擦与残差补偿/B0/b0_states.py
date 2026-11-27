"""Time alignment, causal reconstruction and frozen-model evaluation for B0."""
import numpy as np
import pandas as pd

from b0_sources import MOVING

DT = .005
VEL_IDLE = 50.0
ACCEL_CLASS = 30_000.0
LAG_STEPS = np.arange(-4,5)


def blocks(good, t, available, nominal):
    finite = good & np.isfinite(t) & np.isfinite(available)
    selected=np.flatnonzero(finite)
    if not len(selected):
        return []
    td, ad=np.diff(t[selected]),np.diff(available[selected])
    cuts=np.flatnonzero((np.diff(selected)!=1)|(td<=0)|(ad<=0)|(td>2.5*nominal))+1
    return [part for part in np.split(selected,cuts) if len(part)>=5]


def phase_labels(source, status, v, a, direction):
    phase = np.full(len(v),"unclassified",dtype="<U18")
    stationary=(status&MOVING)==0
    motion=np.isin(source,["scan","approach","watch_motion"])
    phase[np.isin(source,["before_trial","approach_start","watch_pre"])]="pre_hold"
    phase[np.isin(source,["hold","turnaround","watch_hold","trial_end"])]="hold"
    phase[source=="transfer"]="transfer"
    phase[source=="read"]="read_only"
    phase[motion]="near_zero"
    track=motion&np.isfinite(v)&np.isfinite(a)
    sign=direction if direction else np.sign(v)
    phase[track&(np.abs(v)>=2000)]="steady"
    phase[track & (sign*a>ACCEL_CLASS)]="acceleration"
    phase[track & (sign*a<-ACCEL_CLASS)]="deceleration"
    phase[(phase=="hold") & (~stationary | (np.abs(v)>VEL_IDLE))]="settling"
    return phase


def friction_and_residual(x,i,v,a,stationary,ip,alpha,f1,grid):
    p_lo,p_hi=ip["range_pulse"]
    in_x=(x>=p_lo)&(x<=p_hi)
    f_x=(x>=grid[0])&(x<=grid[-1])
    finite=np.isfinite(v)&np.isfinite(a)
    speed=np.abs(v)
    sliding=finite&(speed>=2000)&(speed<=40000)&(~stationary)
    stationary=stationary&finite
    near=finite&(~stationary)&(speed<2000)
    above=finite&(speed>40000)
    p=np.where(in_x,np.interp(x,ip["node_pulse"],ip["current_raw"]),np.nan)
    inertia=a*alpha
    f=np.full(x.shape,np.nan)
    for direction in (1,-1):
        mask=sliding & (direction*v>0)
        intercept,slope=f1[str(direction)]
        f[mask]=intercept+slope*speed[mask]/1000
    f[stationary]=0.
    m0=p+inertia
    diagnostic=m0+f
    strict=(stationary|f_x)&np.isfinite(diagnostic)
    m1=np.where(strict,diagnostic,np.nan)
    # Evidence tier is a flag. Alpha is retained as an explicitly marked linear reference outside its calibration.
    atier=np.select([~np.isfinite(a),np.abs(a)<ACCEL_CLASS,np.abs(a)<400000,np.abs(a)<=600000],
                     ["missing","near_steady","below_400k","400k_to_600k"],default="above_600k")
    return dict(I_p_raw=p,I_a_raw=inertia,I_f_raw=f,I_M0_raw=m0,I_mech_raw=m1,
                r_M0_raw=i-m0,r_raw=i-m1,r_F1_position_diagnostic_raw=np.where(
                    sliding&~f_x,i-diagnostic,np.nan),
                ip_position_valid=in_x,f1_position_valid=f_x,f1_speed_valid=sliding,
                zero_friction_hold=stationary,f1_near_zero_unresolved=near,f1_above_40=above,
                alpha_reference_tier=atier,mechanism_domain_valid=strict)


def state(run, indexes, grid, ip, alpha, f1, friction_grid, segment):
    t, available = run.t[indexes], run.available[indexes]
    x_raw=run.x[indexes]
    last=np.searchsorted(available,grid+1e-10,side="right")-1
    last=np.maximum(last,0)
    x=x_raw[last]
    read_time=t[last]
    ready_time=available[last]
    i=run.current[indexes[last]]
    overage=(grid-ready_time)>2.5*run.nominal
    assert np.all(ready_time<=grid+2e-10)
    # Offline reference at the same native current timestamp, using two positions on either side.
    offsets=np.arange(-2,3)*DT
    window=read_time[:,None]+offsets
    ref_valid=(window[:,0]>=t[0]-1e-10)&(window[:,-1]<=t[-1]+1e-10)&(~overage)
    centered=np.interp(window.ravel(),t,x_raw).reshape(-1,5)
    ref_v=np.where(ref_valid,centered@np.array([-2,-1,0,1,2])/(10*DT),np.nan)
    ref_a=np.where(ref_valid,centered@np.array([2,-1,-2,-1,2])/(7*DT**2),np.nan)
    # Host: five distinct native samples. Watch: most recent native samples at past 5 ms offsets.
    if run.nominal >= .0025:
        indices=last[:,None]+np.arange(-4,1)
    else:
        indices=np.searchsorted(t,read_time[:,None]+np.arange(-4,1)*DT+1e-10,side="right")-1
    supported=(indices[:,0]>=0)&np.all(np.diff(indices,axis=1)>0,axis=1)&~overage
    vc,ac=np.full(len(grid),np.nan),np.full(len(grid),np.nan)
    if supported.any():
        ii=indices[supported]
        assert np.max(ii-last[supported,None])<=0
        tx=(t[ii]-read_time[supported,None])/DT
        design=np.stack((np.ones_like(tx),tx,tx**2),axis=2)
        trans=np.swapaxes(design,1,2)
        rhs=(x_raw[ii]-x[supported,None])[...,None]
        fit=np.linalg.solve(trans@design,trans@rhs)[...,0]
        vc[supported]=fit[:,1]/DT
        ac[supported]=2*fit[:,2]/DT**2
    physical=run.stage[indexes[last]]
    status=run.status[indexes[last]]
    phase=phase_labels(physical,status,ref_v,ref_a,run.record["direction"])
    # Hold is a source-backed state. Never substitute the zero branch during moving near-zero samples.
    eligible_hold=((status&MOVING)==0)&np.isin(physical,["hold","turnaround","watch_hold","watch_pre",
                                                         "before_trial","approach_start","trial_end"])
    ref_idle=eligible_hold&(np.abs(ref_v)<=VEL_IDLE)
    causal_idle=eligible_hold&(np.abs(vc)<=VEL_IDLE)
    ref=friction_and_residual(x,i,ref_v,ref_a,ref_idle,ip,alpha,f1,friction_grid)
    causal=friction_and_residual(x,i,vc,ac,causal_idle,ip,alpha,f1,friction_grid)
    t0=run.t[0]
    history_v=np.full(len(grid),np.nan)
    stopped=np.zeros(len(grid))
    for j in range(len(grid)):
        if not np.isfinite(vc[j]):
            continue
        if j and np.isfinite(history_v[j-1]):
            delta=grid[j]-grid[j-1]
            k=-np.expm1(-delta/.05)
            history_v[j]=history_v[j-1]+k*(vc[j]-history_v[j-1])
            if causal_idle[j] and causal_idle[j-1]:
                stopped[j]=stopped[j-1]+delta
        else:
            history_v[j]=vc[j]
    values=dict(t_grid_s=grid-t0,t_native_s=read_time-t0,
                t_available_s=ready_time-t0,current_source_age_ms=(grid-read_time)*1000,
                segment=segment,source_stage=physical,phase=phase,status_word=status,
                frame_valid=~overage,x_pulse=x,I_actual_raw=i,v_ref_pulse_s=ref_v,
                a_ref_pulse_s2=ref_a,v_causal_pulse_s=vc,a_causal_pulse_s2=ac,
                causal_history_v_pulse_s=history_v,causal_continuous_hold_s=stopped,
                causal_segment_age_s=grid-grid[0])
    values.update({f"ref_{k}":a for k,a in ref.items()})
    values.update({f"causal_{k}":a for k,a in causal.items()})
    return pd.DataFrame(values)


def reconstruct(run,ip,alpha,f1,friction_grid):
    t=run.t
    if not len(t) or not np.isfinite(t[0]):
        return pd.DataFrame()
    frames=[]
    for segment, ix in enumerate(blocks(run.good,t,run.available,run.nominal),1):
        # Use the same session-relative 5 ms grid on both sides of any raw discontinuity.
        start=int(np.ceil((run.available[ix[0]]-t[0])/DT-1e-9))
        stop=int(np.floor((run.available[ix[-1]]-t[0])/DT+1e-9))
        grid=t[0]+np.arange(start,stop+1)*DT
        if len(grid)>=5:
            frames.append(state(run,ix,grid,ip,alpha,f1,friction_grid,segment))
    return pd.concat(frames,ignore_index=True) if frames else pd.DataFrame()


def longest_observed(mask,t,segments):
    j=np.flatnonzero(mask)
    if not len(j): return 0.
    cuts=np.flatnonzero((np.diff(j)!=1)|(segments[j[1:]]!=segments[j[:-1]]))+1
    return max(float(t[z[-1]]-t[z[0]]) for z in np.split(j,cuts))


def lag_audit(df,record):
    """Balanced support: same current observations for every shift, on one uninterrupted segment."""
    offsets=[]
    for seg in df["segment"].unique():
        sample=df[df["segment"].eq(seg)]
        if len(sample)<13:
            continue
        take=np.arange(4,len(sample)-4)
        a=sample["ref_I_a_raw"].to_numpy()
        ac=sample["causal_I_a_raw"].to_numpy()
        truth=(sample["I_actual_raw"]-sample["ref_I_p_raw"]-sample["ref_I_f_raw"]).to_numpy()
        av=a[take[:,None]+LAG_STEPS]
        center=sample["phase"].isin(["acceleration","deceleration"]).to_numpy()[take]
        valid=center & sample["ref_mechanism_domain_valid"].to_numpy()[take] & np.isfinite(truth[take])
        valid &= np.isfinite(av).all(axis=1)&np.isfinite(ac[take])
        if valid.any():
            offsets.append((truth[take][valid],av[valid,:],ac[take][valid]))
    if not offsets:
        return []
    y,av,ac=[np.concatenate(v) for v in zip(*offsets)]
    if len(y)<8 or np.std(av[:,4])<2 or np.std(y)<.1:
        return []
    # Remove only the per-run constant for lag sensitivity. Absolute performance is reported separately.
    err=y[:,None]-av
    centered=err-err.mean(axis=0)
    ac_err=y-ac
    return [dict(run_id=record["run_id"],pair_id=record["pair_id"],cohort=record["cohort"],
                 session_id=record["session_id"],dataset_role=record["dataset_role"],samples=len(y),
                 shift_ms=int(step*5),absolute_rmse_raw=np.sqrt(np.mean(err[:,j]**2)),
                 centered_rmse_raw=np.sqrt(np.mean(centered[:,j]**2)),
                 causal_absolute_rmse_raw=np.sqrt(np.mean(ac_err**2)),
                 ref_causal_a_rms_mm_s2=np.sqrt(np.mean((ac-av[:,4])**2))/0.030306147541113107)
            for j,step in enumerate(LAG_STEPS)]
