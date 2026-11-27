"""Read-only provenance and source adapters for stage 3 B0."""
from dataclasses import dataclass
from functools import cache
import hashlib
import json
import subprocess
from pathlib import Path

import numpy as np
import pandas as pd

OUT = Path(__file__).resolve().parent
STAGES = OUT.parent.parent
REPO = STAGES.parent
S1 = STAGES / "01_位置相关空载电流辨识"
S2 = STAGES / "02_等效惯性电流辨识"
S3 = OUT.parent
POSITION = S1 / "models/position_B_primary_v1.json"
ALPHA = S2 / "results/d/Z-DP_20261126-095126_343_29884_summary.json"
A_ID = "Z-AF-20261126-03"
FRICTION = S3 / "manifest/formal_a" / A_ID / "models_frozen.json"
PLAN_NAME = "ZR_Stage3_Experiment_B_Plan_V1.1.md"
PLAN_SHA = "3121767d6394e26c2926d1e9ccb134a6e7254c7f89f702ee717e2073156d8aa1"
EXT_B = "Z-BP_20261125-195157_221_35296"
D_MANIFEST = S2 / "manifest/d/Z-DP_20261126-095126_343_29884.txt"
B_MANIFEST = S2 / "manifest/b" / (EXT_B + ".txt")
OLD_B = "Z-BP_20261125-155725_864_47920"
FAULT = (1 << 1) | (1 << 5) | (1 << 6)
MOVING = 1 << 10
ENABLE = 1 << 9


def relative(p):
    return p.relative_to(REPO).as_posix() if p else ""


@cache
def sha(path):
    if not path or not path.is_file():
        return ""
    with path.open("rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()


def load(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def csv(path):
    return pd.read_csv(path, low_memory=False)


def fields(path):
    if not path.is_file():
        return {}
    return dict(line.split("=", 1) for line in path.read_text(encoding="utf-8-sig", errors="replace")
                .splitlines() if "=" in line)


def num(value, default=np.nan):
    try:
        v = float(value)
        return v if np.isfinite(v) else default
    except (TypeError, ValueError):
        return default


def paths(record, **sources):
    for name, path in sources.items():
        record[name + "_path"] = relative(path) if path and path.is_file() else ""
        record[name + "_sha256"] = sha(path)
    return record


def baseline():
    ip, inertia, friction = map(load, (POSITION, ALPHA, FRICTION))
    fp = FRICTION.parent / "protocol.json"
    fp_hash = FRICTION.parent / "protocol.sha256"
    assert sha(fp) == fp_hash.read_text(encoding="ascii").strip() == friction["protocol_sha256"]
    f1_protocol = load(fp)
    assert f1_protocol["hashes"]["position"] == sha(POSITION) and f1_protocol["hashes"]["inertia"] == sha(ALPHA)
    assert float(f1_protocol["alpha"]) == float(inertia["alpha"])
    assert inertia["status"] == "passed" and inertia["accepted"] == inertia["planned"] == 5
    assert ip["role"] == "B_primary" and not ip["extrapolation"]
    # Stage 2 records the SHA-256 of the JS-canonical table, while A freezes the complete JSON file.
    check = ("const fs=require('fs'),crypto=require('crypto');"
             "const {table_sha256,...table}=JSON.parse(fs.readFileSync(process.argv[1],'utf8'));"
             "console.log(crypto.createHash('sha256').update(JSON.stringify(table)).digest('hex'));")
    canonical = subprocess.run(["node","-e",check,str(POSITION)],capture_output=True,
                               text=True,check=True).stdout.strip()
    assert canonical == ip["table_sha256"] == fields(D_MANIFEST)["model_sha256"] == fields(B_MANIFEST)["model_sha256"]
    assert fields(D_MANIFEST)["b_protocol_sha256"] == sha(B_MANIFEST)
    for session, expected in ip["raw_sha256"].items():
        assert sha(S1 / "data/raw" / (session + ".csv")) == expected, session
    return ip, float(inertia["alpha"]), friction["models"]["F1"], f1_protocol["grid"]


def formal_acceptance():
    result = {}
    formal = S3 / "manifest/formal_a" / A_ID
    expected_pairs = set(csv(S3 / "results/formal_a" / A_ID / "pairs.csv")["pair"])
    seen = set()
    development = set()
    for f in sorted(formal.glob("attempts/*/acceptance.json")):
        doc = load(f)
        if not doc["accepted"]:
            continue
        for p, h in doc["inputs"].items():
            assert sha(Path(p)) == h, p
        inherited = doc.get("inherited_from")
        if inherited:
            source = S3 / "manifest/formal_a" / inherited["protocol"] / "attempts" / inherited["attempt"] / "acceptance.json"
            assert sha(source) == inherited["acceptance_sha256"]
        for p in doc["accepted"]:
            assert p["pair"] not in seen
            seen.add(p["pair"])
            if p["split"] == "development":
                development.add(p["pair"])
            for leg in p["profiles"].values():
                key = (p["session"], str(leg["trial"]))
                assert key not in result
                result[key] = p
    assert seen == expected_pairs and len(result) == 170
    assert development == set(load(FRICTION)["development_pairs"])
    return result


def watch_membership():
    sources = (
        (S2 / "results/b" / f"{EXT_B}_pairs.csv", "S2_B_expanded", 80),
        (S2 / "results/b" / f"{OLD_B}_pairs.csv", "S2_B_original", 20),
        (S2 / "results/d" / "Z-DP_20261126-095126_343_29884_pairs.csv", "S2_D_final", 5),
        (S2 / "results/d" / "Z-DP_20261126-093024_940_1908_pairs.csv", "S2_D_superseded", None),
    )
    runs = {}
    for path, cohort, accepted in sources:
        df = csv(path)
        if accepted is not None:
            assert df["valid"].eq(1).sum() == accepted
        for row in df.to_dict("records"):
            for role in ("accel", "decel"):
                run = row.get(role + "_run_id")
                if not isinstance(run, str) or not run:
                    continue
                if run in runs:
                    assert runs[run]["cohort"] == cohort
                    if num(row["valid"]) != 1:
                        continue
                runs[run] = dict(cohort=cohort, entry=row, evidence=path)
    a1=S2/"results/b/A1_B1_summary.json"
    qualification=load(a1)
    validated=0
    for session in qualification["ids"]:
        table=S2/"results/a1"/f"{session}_pairs.csv"
        for row in csv(table).to_dict("records"):
            if row.get("session_id") != session or num(row.get("valid")) != 1:
                continue
            validated+=1
            for leg in (1,2):
                run=f"{session}_p{row['pair_id']}_r{leg}"
                runs[run]=dict(cohort="S2_A1",entry=row,evidence=table)
    assert validated==qualification["pairs"]==5
    return runs


@dataclass
class Run:
    record: dict
    t: np.ndarray
    available: np.ndarray
    x: np.ndarray
    current: np.ndarray
    read_duration: np.ndarray
    status: np.ndarray
    stage: np.ndarray
    good: np.ndarray
    nominal: float


def origin_role(source, cohort, phase, use, accepted, archived):
    if archived or not accepted:
        return "diagnostic"
    if source == "stage1":
        if cohort == "S1_C" and use == "development":
            return "train_candidate"
        if cohort == "S1_B_primary" and use == "train":
            return "hold_candidate"
        return "diagnostic"
    if source == "stage2":
        return "train_candidate" if cohort in ("S2_B_expanded", "S2_B_original") else "diagnostic"
    # Earlier model evaluation is available for method development only.
    return "train_candidate" if phase == "AF" and use == "development" else "diagnostic"


def host_runs(base, source, ledger, input_files):
    seen_hash = {}
    for storage in (base, base / "archive"):
        rawdir, mandir = storage / "data/raw", storage / "manifest"
        rawpaths = sorted(rawdir.glob("*.csv"))
        sessions = {p.stem for p in rawpaths} | {p.name.removesuffix(".trials.csv") for p in mandir.glob("*.trials.csv")}
        archived = storage != base
        for session in sorted(sessions):
            raw = rawdir / (session + ".csv")
            manifest = mandir / (session + ".txt")
            trial_path = mandir / (session + ".trials.csv")
            plan_path = mandir / (session + ".plan.csv")
            input_files.update(p for p in (raw, manifest, trial_path, plan_path) if p.is_file())
            md = fields(manifest)
            phase = md.get("phase", "")
            use_files = dict(raw=raw, manifest=manifest, trials=trial_path, plan=plan_path)
            empty = pd.DataFrame()
            df = csv(raw) if raw.is_file() and raw.stat().st_size else empty
            if not df.empty and "trial_id" not in df:
                input_files.add(raw)
                yield paths(dict(source=source, source_class="unsupported_host_csv", session_id=session,
                                 run_id=f"{source}:{session}:file", dataset_role="diagnostic", native_rows=len(df)),
                            **use_files), None
                continue
            groups = {str(k): g for k, g in df.groupby("trial_id", sort=True)} if not df.empty else {}
            def trial_rows(path):
                return {str(t["trial_id"]): t for t in csv(path).to_dict("records")} if path.is_file() else {}
            trials, plan = trial_rows(trial_path), trial_rows(plan_path)
            h = sha(raw)
            duplicate = seen_hash.get(h, "") if h else ""
            if h and not duplicate:
                seen_hash[h] = session
            if source == "stage1":
                if phase == "B" and "170633" in session:
                    cohort = "S1_B_primary"
                else:
                    cohort = "S1_" + (session.split("_")[0].removeprefix("Z-") if phase == "D" else phase or "unknown")
            else:
                cohort = "S3_A_final" if phase == "AF" else "S3_A0"
            ids = sorted(set(groups) | set(trials) | set(plan), key=lambda k: int(k))
            for k in ids or ["0"]:
                t = trials.get(k, {})
                planned = plan.get(k, {})
                g = groups.get(k, empty)
                a = ledger.get((session, k), {})
                fields_ok = all(str(t.get(x)) == str(v) for x, v in planned.items() if x != "trial_id")
                host_config = (md.get("current_api") == "GTN_GetAtlTorque" and md.get("current_unit") == "raw"
                               and num(md.get("pulse_per_mm")) == 1000 and
                               md.get("config") == "a33_core2_axis1" and md.get("position_mode") == "2"
                               and md.get("R_state") == "R_fixed_operator_confirmed")
                use = str(t.get("use", planned.get("use", "")))
                direction = int(num(t.get("direction", planned.get("direction")), 0))
                speed = num(t.get("speed_mm_s", planned.get("speed_mm_s")))
                group = str(t.get("group", planned.get("group", "")))
                key = (a.get("pair") or (f"{group}:{speed:g}" if phase in ("C","A0F","AF") else k))
                pair_id = ("AF:" + key) if a else f"{source}:{session}:{key}"
                # Unaccepted formal attempts do not enter the final A pair ledger.
                quality = bool(not g.empty and t.get("status") == "completed" and fields_ok and not duplicate
                               and host_config and (bool(a) if phase == "AF" else md.get("outcome") == "completed"))
                role = origin_role(source, cohort, phase, use, quality, archived)
                hold = use in ("train", "unseen", "cross_time") and phase in ("B", "D")
                used = ("Ip_fit" if cohort == "S1_B_primary" and use == "train" else
                        "Ip_validation" if phase == "D" else
                        "position_C_overlap_or_reference" if phase == "C" else
                        "F1_fit" if a and use == "development" else
                        "F1_speed_validation" if a and use == "speed_validation" else
                        "F1_independent_evaluation" if a else "none_recorded")
                data_session = a.get("field_session") or session
                # The date-based group is intentionally conservative for records with no field session ID.
                field_group = a.get("field_session") or session.split("_")[1][:8] if "_" in session else data_session
                period = num(t.get("period_s"), num(md.get("period_s"), .005))
                rec = paths(dict(source=source, source_class="host_csv", cohort=cohort,
                                 session_id=session, field_session=data_session, split_group=field_group,
                                 run_id=f"{source}:{session}:{k}", pair_id=pair_id, trial_id=k,
                                 group=group, phase_source=phase, source_use=use, dataset_role=role,
                                 prior_model_use=used, historical_only=True, archive=archived,
                                 final_E=False, raw_quality=quality, clock="host_monotonic_pre_read_s",
                                 duplicate_source=duplicate, status=t.get("status", "no_trial_record"),
                                 reason=t.get("reason", ""), parent_outcome=md.get("outcome",""),
                                 plan_agrees=bool(planned) and fields_ok, configuration_matches=host_config,
                                 controller_mode=md.get("position_mode",""), scale_pulse_per_mm=num(md.get("pulse_per_mm")),
                                 firmware=md.get("firmware_version",""), current_signal=md.get("current_api",""),
                                 direction=direction, nominal_speed_mm_s=speed,
                                 nominal_accel_mm_s2=num(md.get("acc_mm_s2")),
                                 commanded_start_mm=num(t.get("approach_mm", planned.get("approach_mm"))),
                                 commanded_end_mm=num(t.get("target_mm", planned.get("target_mm"))),
                                 pre_hold_s_nominal=np.nan,
                                 post_hold_s_nominal=num(t.get("hold_s"),num(md.get("hold_s"))),
                                 nominal_period_ms=period*1000, native_rows=len(g), has_hold=hold,
                                 source_event="host_stages", foreground_run_id=str(k)), **use_files)
                if g.empty or not host_config or duplicate:
                    yield rec,None
                    continue
                columns = ("t_s","read_s","x_mm","current_fb_raw","status","valid","alarm","enabled","mode","limit_pos","limit_neg","planner")
                if not set(columns).issubset(g.columns):
                    yield rec,None
                    continue
                tt, read, x, i, st, valid, alarm, en, mode, lp, ln, planner = g[list(columns)].to_numpy(dtype=float).T
                st = np.nan_to_num(st, nan=-1).astype(np.int64)
                rawgood=(np.isfinite(tt)&np.isfinite(x)&np.isfinite(i)&np.isfinite(read)&(read>=0)&
                         (valid==1)&(alarm==0)&(en==1)&(mode==2)&(lp==0)&(ln==0)&(planner==0)&
                         ((st&FAULT)==0))
                # Host raw timestamp precedes sequential SDK reads. Availability is no earlier than read completion.
                yield rec,Run(rec,tt,tt+read,x*1000,i,read,st,g["stage"].astype(str).to_numpy(),rawgood,period)


def unwrap_ticks(ticks):
    delta = np.diff(ticks)
    return np.r_[0,np.cumsum(np.where(delta < -(1<<31),delta+(1<<32),delta))]


def watch_runs(membership, input_files, seen_sidecars):
    rawdir, mandir = S2 / "data/raw", S2 / "manifest"
    seen={}
    for raw in sorted(rawdir.glob("*.csv"),key=lambda p:(p.stem not in membership,p.name)):
        if raw.name.endswith((".sdk.csv", ".batches.csv")):
            input_files.add(raw)
            seen_sidecars.append(raw)
            continue
        run = raw.stem
        signature=sha(raw)
        duplicate=seen.setdefault(signature,run)
        duplicate="" if duplicate==run else duplicate
        mf = mandir / (run+".txt")
        md = fields(mf)
        parent = mandir / (md.get("session_id","")+".txt")
        pm = fields(parent)
        evidence = membership.get(run, {})
        pr = evidence.get("entry", {})
        session = md.get("session_id",run)
        period = num(md.get("core_period_ns"),num(pm.get("core_period_ns")))*1e-9
        cohort = evidence.get("cohort", "S2_A1" if run.startswith("Z-A1_") else
                              "S2_Watch_probe" if run.startswith("Z-HS_") else "S2_other")
        run_config = pm or md
        configured = (run_config.get("core","2")=="2" and run_config.get("axis","1")=="1" and
                      (run_config.get("config","a33_core2_axis1")=="a33_core2_axis1") and
                      num(run_config.get("scale",1000))==1000 and
                      num(run_config.get("reference_before.control_mode",2))==2)
        sdk_path=rawdir/(run+".sdk.csv")
        batch_path=rawdir/(run+".batches.csv")
        ev=evidence.get("evidence")
        qs=csv(raw)
        input_files.update(p for p in (raw,mf,parent,ev,sdk_path,batch_path) if p and p.is_file())
        normal = {"frame","card_tick_raw","encoder_position_raw","current_feedback_raw"} <= set(qs)
        duration_invalid = pd.DataFrame()
        if sdk_path.is_file() and sdk_path.stat().st_size:
            duration_invalid=csv(sdk_path)
        sdk_error=np.nan
        sdk_max_gap=np.nan
        sdk_p95=np.nan
        if not duration_invalid.empty and "status" in duration_invalid:
            codes=[c for c in duration_invalid if c.startswith("rc_")]
            s=duration_invalid["status"].fillna(-1).to_numpy(dtype=np.int64)
            rc=duration_invalid[codes].fillna(-1).ne(0).any(axis=1).to_numpy()
            sdk_error=int((rc|((s&FAULT)!=0)|((s&ENABLE)==0)).sum())
            if {"host_begin_us","host_end_us"} <= set(duration_invalid):
                span=(duration_invalid["host_end_us"]-duration_invalid["host_begin_us"])/1000
                sdk_p95=float(span.quantile(.95))
                sdk_max_gap=float(duration_invalid["host_begin_us"].diff().max()/1000)
        watch_good=normal and np.isfinite(period) and period>0
        if watch_good and len(qs):
            ticks=qs["card_tick_raw"].to_numpy(dtype=np.int64)
            uu=unwrap_ticks(ticks)
            contiguous=bool(np.all(np.diff(uu)==1) and np.all(np.diff(qs["frame"].to_numpy())==1))
        else:
            contiguous=False
        event=md.get("command_before_tick")
        quality=bool(not duplicate and watch_good and contiguous and configured and md.get("outcome")=="complete"
                     and md.get("watch_aligned_contiguous")=="1" and sdk_error==0 and
                     num(pr.get("valid"))==1)
        role=origin_role("stage2",cohort,"watch","matched",quality,False)
        pair=pr.get("planned_pair_id") or f"{session}:p{md.get('pair_id','?')}"
        actual_cmd=num(md.get("run_speed_readback_pulse_ms"),num(pm.get("peak_pulse_s"))/1000)
        if "Z-A1" in run:
            used="alpha_method_development"
        elif cohort=="S2_B_expanded":
            used="alpha_estimation_or_tier_validation"
        elif cohort=="S2_B_original":
            used="alpha_method_development"
        elif cohort=="S2_D_final":
            used="alpha_independent_evaluation"
        else:
            used="none_recorded"
        record=paths(dict(source="stage2",source_class="watch_csv",cohort=cohort,session_id=session,
                          field_session=session,split_group=session.split("_")[1][:8] if "_" in session else session,
                          run_id=f"stage2:{run}",pair_id=f"stage2:{pair}",trial_id=run,group=str(md.get("round","")),
                          phase_source=md.get("role","watch"),source_use="matched_"+md.get("role","probe"),
                          dataset_role=role,prior_model_use=used,historical_only=True,archive=False,final_E=False,
                          raw_quality=quality,clock="common_card_interrupt_tick",configuration_matches=configured,
                          firmware=pm.get("firmware",""),controller_mode=2 if configured else "",
                          scale_pulse_per_mm=1000,current_signal="Watch_atl_torque_feedback_raw",
                          duplicate_source=duplicate,status=md.get("outcome","unclassified"),reason=md.get("reason",""),
                          card_first_tick=int(qs["card_tick_raw"].iloc[0]) if normal and len(qs) else np.nan,
                          command_before_tick=num(event),command_after_tick=num(md.get("command_after_tick")),
                          settled_tick=num(md.get("settled_tick")),
                          source_group_valid=num(pr.get("valid")),plan_agrees=bool(ev),
                          parent_outcome=pm.get("outcome",""),direction=int(num(md.get("direction"),0)),
                          nominal_speed_mm_s=actual_cmd, match_speed_mm_s=num(pr.get("nominal_v_pulse_s"))/1000,
                          nominal_accel_mm_s2=num(pr.get("nominal_A_pulse_s2"),
                                                 num(md.get("run_trap_requested_acc_pulse_ms2"))*1000)/1000
                            if np.isfinite(num(pr.get("nominal_A_pulse_s2"))) else num(md.get("run_trap_requested_acc_pulse_ms2"))*1000,
                          commanded_start_mm=num(md.get("start_mm")),commanded_end_mm=num(md.get("end_mm")),
                          pre_hold_s_nominal=num(md.get("preparation_hold_ms"))/1000,
                          post_hold_s_nominal=np.nan,nominal_period_ms=period*1000,native_rows=len(qs),
                          source_event="card_ticks" if event else "no_command_anchor",
                          watch_contiguous=contiguous,sdk_fault_or_failed_polls=sdk_error,
                          sdk_poll_span_p95_ms=sdk_p95,sdk_max_poll_gap_ms=sdk_max_gap,
                          match_condition=pr.get("condition",""),execution_role=md.get("role","")),
                     raw=raw,manifest=mf,session_manifest=parent,upstream_acceptance=ev,sdk=sdk_path,read_batches=batch_path)
        if not watch_good or not len(qs) or duplicate:
            yield record,None
            continue
        tt=uu*period
        def event_time(name):
            val=num(md.get(name))
            offset=(val-int(ticks[0]))%(1<<32) if np.isfinite(val) else np.nan
            offset=offset-(1<<32) if offset>(1<<31) else offset
            return offset*period
        start, settled=event_time("command_before_tick"),event_time("settled_tick")
        record["command_bracket_ms"]=(event_time("command_after_tick")-start)*1000
        record["pre_observed_s"]=max(0.,start-tt[0]) if np.isfinite(start) else np.nan
        record["post_observed_s"]=max(0.,tt[-1]-settled) if np.isfinite(settled) else np.nan
        if np.isfinite(start) and np.isfinite(settled):
            stage=np.where(tt<start,"watch_pre",np.where(tt<settled,"watch_motion","watch_hold"))
        else:
            stage=np.full(len(tt),"watch_unknown")
        x,i=qs[["encoder_position_raw","current_feedback_raw"]].to_numpy(dtype=float).T
        # Watch position and current share the frame tick. SDK status is a separate sampled channel.
        st=np.where(stage=="watch_motion",ENABLE|MOVING,ENABLE).astype(np.int64)
        valid=np.isfinite(tt)&np.isfinite(x)&np.isfinite(i)&configured
        if not np.isnan(sdk_error) and sdk_error:
            valid[:]=False
        yield record,Run(record,tt,tt,x,i,np.zeros(len(tt)),st,stage,valid,period)
