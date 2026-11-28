"""Causal position-only estimation and frozen mechanism replay."""
from common import *


def load_grid():
    t = v2.dataset()
    assert file_hash(DATA) == read(V1 / "training_config.json")["dataset_sha256"]
    assert t.cohort.isin(["B1", "C", "D"]).all()
    for ix in groups(t):
        assert np.array_equal(t.frame[ix], np.arange(len(ix)) * 100)
    return t


def s2(x, v, a):
    cfg = P["state"]
    vel, acc = v.copy(), a.copy()
    quiet_count = 0
    stationary = False
    anchor = x[0]
    for i in range(cfg["s2_window_grids"], len(x)):
        last = x[i] - x[i-1]
        recent = x[i] - x[i-2]
        span = np.ptp(x[i-4:i+1])
        quiet = (abs(last) <= cfg["s2_stationary_5ms_pulse"]
                 and span <= cfg["s2_stationary_20ms_pulse"])
        quiet_count = quiet_count + 1 if quiet else 0
        if stationary and abs(x[i] - anchor) >= cfg["s2_exit_displacement_pulse"]:
            stationary = False
            quiet_count = 0
        if not stationary and quiet_count >= cfg["s2_enter_quiet_samples"]:
            stationary = True
            anchor = x[i]
        if stationary:
            vel[i], acc[i] = 0, 0
        elif v[i] * recent <= 0:
            # Latest measurable drift supports a new direction, including reversal.
            vel[i] = recent / .010 if abs(recent) >= cfg["s2_direction_10ms_pulse"] else last / .005
            if abs(last) <= 1 and abs(recent) < cfg["s2_direction_10ms_pulse"]:
                vel[i] = 0
    return vel, acc


def matrices(dt, qj):
    a = np.array([[1., dt, dt**2/2], [0., 1., dt], [0., 0., 1.]])
    q = qj * np.array([[dt**5/20, dt**4/8, dt**3/6],
                       [dt**4/8, dt**3/3, dt**2/2],
                       [dt**3/6, dt**2/2, dt]])
    return a, q


def gains(n, r, qj):
    dt = P["ts_s"]
    a, q = matrices(dt, qj)
    cov = np.diag([r, 200**2, 30000**2])
    result = np.zeros((n, 3))
    for i in range(1, n):
        prior = a @ cov @ a.T + q
        k = prior[:, 0] / (prior[0, 0] + r)
        correction = np.eye(3)
        correction[:, 0] -= k
        cov = correction @ prior @ correction.T + r * np.outer(k, k)
        result[i] = k
    return result


def s3(x, gain):
    dt = P["ts_s"]
    pos, v, a = x[0], 0., 0.
    va = np.zeros((len(x), 2))
    for i in range(1, len(x)):
        pos += dt*v + dt**2/2*a
        v += dt*a
        innovation = x[i] - pos
        kx, kv, ka = gain[i]
        pos += kx * innovation
        v += kv * innovation
        a += ka * innovation
        va[i] = v, a
    return va.T


def estimate(t, name, r_floor=None):
    start = perf_counter()
    x, v, a = t[XVA].to_numpy().T
    if name == "S0":
        return v, a, 0.
    blocks = list(groups(t))
    if name == "S2":
        impl = lambda ix: s2(x[ix], v[ix], a[ix])
    else:
        _, scale, qj = name.split("_")
        gain = gains(max(map(len, blocks)), r_floor * float(scale), float(qj))
        impl = lambda ix: s3(x[ix], gain)
    result = np.zeros((2, len(x)))
    for ix in blocks:
        result[:, ix] = impl(ix)
    # Slice-prefix equality verifies restart and causal implementation.
    ix = blocks[0]
    half = ix[:len(ix)//2]
    assert np.allclose(impl(half), result[:, half], equal_nan=True)
    return *result, (perf_counter() - start) / len(t) * 1e6


def effective_r(t, train):
    cfg = P["state"]
    stable = train & t.hold_zero.to_numpy() & t.hold_age_s.ge(.10).to_numpy()
    tr = t.loc[stable]
    var = tr.groupby(["trace", "segment"]).x_pulse.var().dropna().to_numpy()
    return max(cfg["r_quantization_floor"], float(np.quantile(var, cfg["r_static_percentile"])))


def mechanism(t, v, a, baseline=False):
    if baseline:
        return t.ia_raw.to_numpy(), t.if_raw.to_numpy(), t.m1_raw.to_numpy(), t.candidate.to_numpy()
    cfg = read(prep.ROOT / "B0/audit_provenance.json")
    assert file_hash(prep.ALPHA) == cfg["alpha_source_sha256"]
    assert file_hash(prep.FRICTION) == cfg["friction_model_sha256"]
    alpha = read(prep.ALPHA)["alpha"]
    f1 = read(prep.FRICTION)["models"]["F1"]
    # Preserve the frozen replay's static convention for an isolated A comparison.
    still = t.hold_zero.to_numpy()
    motion = t.phase.isin([*LOADING, "near_zero", "startup"]).to_numpy()
    ia = np.where(still, 0., alpha * a)
    slide = (motion & t.f1_position_valid.to_numpy()
             & (np.abs(v) >= 2000) & (np.abs(v) <= 40000))
    f = np.zeros(len(t))
    for direction in (-1, 1):
        inc = slide & (direction*v > 0)
        offset, slope = f1[str(direction)]
        f[inc] = offset + slope * direction*v[inc]/1000
    total = t.ip_raw.to_numpy() + ia + f
    core = (t.history_known.to_numpy() & ~t.short_ramp.to_numpy() & t.cohort.ne("D").to_numpy()
            & ((np.abs(a) < 30000) | ((np.abs(a) >= 390000) & (np.abs(a) <= 610000))))
    valid = np.isfinite(total) & (still | (slide & core))
    return ia, f, total, valid


def transformed(t, name, r_floor):
    v, a, cost = estimate(t, name, r_floor)
    ia, fr, total, valid = mechanism(t, v, a, name == "S0")
    if name == "S0":
        return t, valid, cost
    out = t.copy()
    out["v_pulse_s"], out["a_pulse_s2"] = v, a
    for ix in groups(t):
        x = t.x_pulse.to_numpy()[ix]
        features = np.asarray(prep.history_features(x, v[ix])).T
        out.loc[ix, base.FEATURES[3:]] = features
        out.loc[ix, ["arrival_speed", "travel"]] = v2.features(x, v[ix], a[ix], features[:, -1])[:, 1:3]
    out["ia_raw"], out["if_raw"], out["m1_raw"] = ia, fr, total
    out["label_raw"] = np.where(t.candidate, t.current_raw.to_numpy() - total, np.nan)
    assert np.isfinite(out.loc[t.candidate, EXPLICIT + ["label_raw"]]).all().all()
    return out, valid, cost


def state_names():
    return ["S0", "S2", *(f"S3_{r:g}_{q:g}" for r in P["state"]["r_multipliers"]
                         for q in P["state"]["qj"])]

