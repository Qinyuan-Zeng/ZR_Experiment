"""Offline B1 pair acceptance on frozen Watch, stage events and host feedback."""
from pathlib import Path

import numpy as np
import pandas as pd

from a0_replay import digest, require

TICK_NS = 50_000
MODULUS = 1 << 32

def fields(path):
    return dict(line.split("=",1) for line in path.read_text(encoding="utf-8-sig").splitlines() if "=" in line)


def check_leg(folder,slot,leg,limits):
    prefix=folder/f'{slot["id"]}_leg{leg}'
    paths={key:Path(str(prefix)+extension) for key,extension in
           (("watch",".csv"),("metadata",".txt"),("host",".sdk.csv"),("batches",".batches.csv"))}
    hashes={name:digest(path) for name,path in paths.items()}
    meta=fields(paths["metadata"])
    sign=slot["first"] if leg==1 else -slot["first"]
    start,end=(slot["lo"],slot["hi"]) if sign==1 else (slot["hi"],slot["lo"])
    require(meta["schema"]=="b1_watch_v1" and meta["slot_id"]==slot["id"] and int(meta["leg"])==leg
            and int(meta["direction"])==sign and int(meta["start_pulse"])==start
            and int(meta["end_pulse"])==end and int(meta["nominal_speed_mm_s"])==slot["v"]
            and int(meta["nominal_acceleration_mm_s2"])==slot["a"]
            and int(meta["core_period_ns"])==int(meta["watch_expected_frame_ns"])==TICK_NS
            and meta["outcome"]=="complete" and meta["watch_aligned_contiguous"]=="1",
            f"Leg {leg}: incomplete acquisition or mismatched configuration")
    raw=pd.read_csv(paths["watch"])
    require(list(raw.columns)==["frame","card_tick_raw","encoder_position_raw","current_feedback_raw"] and len(raw)>40000,
            f"Leg {leg}: raw Watch shape or duration invalid")
    require(np.isfinite(raw.to_numpy(dtype=float)).all(),"Missing Watch observation")
    absolute=raw["card_tick_raw"].to_numpy(dtype=np.int64)
    ticks=(absolute-absolute[0])%MODULUS
    require(np.array_equal(raw["frame"].to_numpy(),np.arange(len(raw))) and np.all(np.diff(ticks)==1),
            f"Leg {leg}: Watch drop, duplicate or unexpected interval")
    before,after,settled,post=((int(meta[k])-int(absolute[0]))%MODULUS for k in
                              ("command_before_tick","command_after_tick","settled_tick","post_hold_complete_tick"))
    hold=int(1e9/TICK_NS)
    # Command brackets, position and current all use the same card tick axis.
    require(hold<=before<=after<settled and settled+hold<=post<=ticks[-1],
            f"Leg {leg}: incomplete one-second holds or missing motion boundary")
    x=raw["encoder_position_raw"].to_numpy()
    current=raw["current_feedback_raw"].to_numpy()
    xmin,xmax=sorted((start,end))
    require(x.min()>=max(limits["lo"],xmin-10) and x.max()<=min(limits["hi"],xmax+10),
            f"Leg {leg}: Watch position exceeded bounds")
    require(np.abs(x[before-hold:before]-start).max()<=10 and np.abs(x[post-hold:post]-end).max()<=10,
            f"Leg {leg}: final pre/post one-second position hold is outside ±10 pulse")
    # Fixed 5 ms secant for the offline speed reach check, finer raw frames are preserved.
    lag=int(5e6/TICK_NS)
    v=sign*(x[lag:]-x[:-lag])/5.0  # pulses per millisecond equal mm/s
    window=(ticks[:-lag]>=after)&(ticks[lag:]<=settled)
    v=v[window]
    require(len(v)>0,f"Leg {leg}: no motion interval supported")
    require(np.abs(v).max()<=limits["max_speed"],f"Leg {leg}: Watch-derived velocity exceeded protection")
    require(np.sum(np.abs(v-slot["v"])<=max(.2,.05*slot["v"]))>=int(15e6/TICK_NS)
            and np.percentile(v,95)>=.90*slot["v"],
            f"Leg {leg}: feedback does not support scheduled speed")
    host=pd.read_csv(paths["host"])
    codes=[k for k in host if k.startswith("rc_")]
    require(len(codes)==9 and host[codes].eq(0).all(axis=None) and
            {"pre_hold","motion","post_hold"}<=set(host["phase"]),f"Leg {leg}: incomplete host state trace")
    motion=host[host["phase"].eq("motion")]
    gap=motion["host_begin_us"].diff().dropna()
    require(len(gap)>0 and gap.gt(0).all(),f"Leg {leg}: invalid host trace timestamps")
    statuses=host["status"].to_numpy(dtype=np.int64)
    faults=(1<<1)|(1<<5)|(1<<6)
    require(np.all((statuses&faults)==0) and np.all((statuses&(1<<9))!=0),
            f"Leg {leg}: recorded host fault/disabled state")
    require(all(digest(path)==hashes[name] for name,path in paths.items()),f"Leg {leg}: source changed during replay")
    stats=dict(native_frames=len(raw),watch_period_us=TICK_NS/1e3,native_current_abs_max=float(np.abs(current).max()),
               motion_speed_p95_mm_s=float(np.percentile(v,95)),motion_duration_s=(settled-after)*TICK_NS/1e9,
               motion_host_interval_p95_ms=float(gap.quantile(.95)/1000),
               command_bracket_ms=(after-before)*TICK_NS/1e6,
               pre_capture_s=before*TICK_NS/1e9,post_capture_s=(post-settled)*TICK_NS/1e9)
    return stats,{name:{"path":str(path),"sha256":hashes[name]} for name,path in paths.items()}


def check_pair(folder,slot,limits):
    values={}
    inputs={}
    for leg in (1,2):
        stats,source=check_leg(folder,slot,leg,limits)
        side="positive" if (slot["first"] if leg==1 else -slot["first"])>0 else "negative"
        values.update({side+"_"+k:v for k,v in stats.items()})
        inputs[side]=source
    return dict(path=slot["path"],speed_mm_s=slot["v"],acc_mm_s2=slot["a"],round=slot["round"],period=slot["period"],
                leg_inputs=inputs,**values)
