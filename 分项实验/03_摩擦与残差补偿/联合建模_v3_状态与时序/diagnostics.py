"""Native Watch is a diagnostic reference only. No labels are inferred for training."""
from common import *


def native_reference(t):
    manifest = pd.read_csv(V1 / "sources.csv").set_index("trace", verify_integrity=True)
    refs, sdk = [], []
    for ix in groups(t):
        rec = manifest.loc[t.trace.iat[ix[0]]]
        locked = json.loads(rec.inputs)
        path = prep.ROOT / rec.raw
        assert file_hash(path) == locked[rec.raw]
        df = pd.read_csv(path, usecols=["encoder_position_raw", "card_tick_raw", "frame"])
        x = df.encoder_position_raw.to_numpy(dtype=float)
        k = t.frame.to_numpy()[ix]
        assert len(x) == rec.frames and np.array_equal(df.frame, np.arange(len(x)))
        assert np.array_equal(x[k], t.x_pulse.to_numpy()[ix])
        assert np.array_equal(df.card_tick_raw.to_numpy()[k], t.card_tick_raw.to_numpy()[ix])
        idx = lambda step: np.clip(k + step, 0, len(x)-1)
        v = (x[idx(100)] - x[idx(-100)]) / .010
        a = (x[idx(200)] - 2*x[k] + x[idx(-200)]) / .010**2
        v[(k < 100) | (k+100 >= len(x))] = np.nan
        a[(k < 200) | (k+200 >= len(x))] = np.nan
        refs.append(pd.DataFrame(dict(trace=rec.name, frame=k, ref_v=v, ref_a=a,
                                      dx_back=x[k]-x[idx(-100)], dx_ahead=x[idx(100)]-x[k])))
        sdk_key = next((f for f in locked if f.endswith(".sdk.csv")), None)
        if sdk_key:
            sdk_path = prep.ROOT / sdk_key
            assert file_hash(sdk_path) == locked[sdk_key]
            side = pd.read_csv(sdk_path)
            vcol = "velocity_pulse_ms"
            both = {"rc_v", vcol, "host_begin_us", "host_end_us", "phase"}
            if both.issubset(side.columns):
                times = side.host_begin_us.to_numpy()
                dt = np.diff(times) / 1000
                duration = (side.host_end_us-side.host_begin_us)/1000
                values = side.loc[side.rc_v.eq(0), vcol].dropna()
                active = side.phase.astype(str).str.contains("motion|approach|probe|move")
                active_dt = dt[active.to_numpy()[:-1] & active.to_numpy()[1:]]
                sdk.append(dict(trace=rec.name, cohort=rec.cohort, rows=len(side),
                                rc_ok=int(side.rc_v.eq(0).sum()), active_rows=int(active.sum()),
                                interval_ms_median=np.median(dt) if len(dt) else np.nan,
                                interval_ms_p95=np.quantile(dt,.95) if len(dt) else np.nan,
                                active_interval_ms_median=np.median(active_dt) if len(active_dt) else np.nan,
                                read_span_ms_median=np.median(duration),
                                velocity_levels=values.nunique(),
                                multiples_20_pulse_ms=int(np.isclose(values%20,0).sum()),
                                enc_velocity_card_timestamp=any("vel" in c and "card" in c and "planner" not in c
                                                                for c in side.columns),
                                phase_names="|".join(side.phase.astype(str).unique())))
    ref = pd.concat(refs, ignore_index=True)
    assert ref[["trace", "frame"]].equals(t[["trace", "frame"]])
    csv("A_S1_sdk_audit.csv", sdk)
    ref.to_pickle(RESULTS / "native_reference.pkl")
    return ref


def episodes(mask):
    edges = np.flatnonzero(np.diff(np.r_[False, mask, False]))
    return zip(edges[::2], edges[1::2])


def responses(v, rv, segment, direction, history):
    """50%-speed crossings on matched native motion segments and explicit C reversal evidence."""
    out = []
    for kind in ("move", "approach", "probe"):
        ix = np.flatnonzero(segment == kind)
        if not len(ix):
            continue
        signed = (-(direction) if kind == "approach" and history == "reverse" else direction)
        ref = signed*rv
        est = signed*v
        peak = np.nanmax(ref[ix])
        if peak < 600:
            continue
        mid = ix[np.nanargmax(ref[ix])]
        level = .5 * peak
        tx = np.arange(max(0, ix[0]-2), min(len(v), ix[-1]+12))
        rising = tx[tx <= mid]
        falling = tx[tx >= mid]
        rr = rising[ref[rising] >= level]
        er = rising[est[rising] >= level]
        # A slow observer may first cross after the reference peak.
        if len(rr) and not len(er):
            er = falling[est[falling] >= level]
        rf = falling[ref[falling] >= level]
        ef = falling[est[falling] >= level]
        app = segment == "approach"
        ref_reverse = kind == "probe" and history == "reverse" and (ref[app] <= -600).sum() >= 2
        seen_reverse = ref_reverse and (est[app] <= -600).sum() >= 2 and (est[ix] >= 600).sum() >= 2
        out.append(dict(segment=kind, reference_peak=peak,
                        rise_delay_ms=5*(er[0]-rr[0]) if len(rr) and len(er) else np.nan,
                        fall_delay_ms=5*(ef[-1]-rf[-1]) if len(rf) and len(ef) else np.nan,
                        rise_expected=bool(len(rr)), fall_expected=bool(len(rf)),
                        true_reversal=ref_reverse, identified_reversal=seen_reverse))
    return out


def inspect(t, ref, v, a, selection, **tags):
    events, timing = [], []
    for ix in groups(t):
        if not selection[ix[0]]:
            continue
        raw = ref.iloc[ix]
        rv = raw.ref_v.to_numpy()
        vv = v[ix]
        backward, ahead = raw.dx_back.to_numpy(), raw.dx_ahead.to_numpy()
        evidence = (np.abs(rv) >= 600) & (rv*vv < 0) & (backward*vv <= 0) & (ahead*vv <= 0)
        # Stop-tail reversal requires strong preceding travel and agreement of native increments.
        prior = pd.Series(rv).shift().rolling(5, min_periods=1)
        preceding = np.where(prior.max().fillna(0).to_numpy() >= 600, 1,
                             np.where(prior.min().fillna(0).to_numpy() <= -600, -1, 0))
        stationary = (np.abs(rv) <= 200) & (np.abs(backward) <= 1) & (np.abs(ahead) <= 1)
        tail = stationary & (preceding*vv < 0)
        mask = (np.abs(vv) >= 600) & (evidence | tail)
        for begin, end in episodes(mask):
            k_peak = begin + np.nanargmax(np.abs(vv[begin:end]))
            frame = t.iloc[ix[k_peak]]
            events.append(dict(tags, trace=frame.trace, unit=frame.unit, cohort=frame.cohort,
                               start_frame=raw.frame.iat[begin], peak_frame=frame.frame,
                               duration_ms=(end-begin)*5, peak_v_pulse_s=vv[k_peak],
                               ref_v_pulse_s=rv[k_peak], dx_back=backward[k_peak], dx_ahead=ahead[k_peak],
                               classification="stop_tail" if tail[k_peak] else "contradicts_native_travel",
                               original_label=bool(t.candidate.iat[ix[k_peak]])))
        g = t.iloc[ix]
        info = dict(tags, trace=g.trace.iat[0], cohort=g.cohort.iat[0], unit=g.unit.iat[0])
        for result in responses(vv, rv, g.segment.to_numpy(), g.direction.iat[0], g.history.iat[0]):
            timing.append(dict(info, **result))
    summary = []
    rv, ra = ref.ref_v.to_numpy(), ref.ref_a.to_numpy()
    for label in ("static", "accelerate", "cruise", "brake", "startup", "near_zero"):
        part = ((t.phase.eq("hold").to_numpy() & t.hold_age_s.ge(.1).to_numpy()
                 & (np.abs(rv) <= 200)) if label == "static" else t.phase.eq(label).to_numpy())
        m = selection & part & np.isfinite(v) & np.isfinite(a) & np.isfinite(rv) & np.isfinite(ra)
        rms = lambda x: float(np.sqrt(np.mean(x*x))) if len(x) else np.nan
        summary.append(dict(tags, phase=label, rows=int(m.sum()), v_rms=rms(v[m]), a_rms=rms(a[m]),
                            v_std=np.std(v[m]), a_std=np.std(a[m]),
                            v_reference_rmse=rms(v[m]-rv[m]), a_reference_rmse=rms(a[m]-ra[m])))
    return events, timing, summary

