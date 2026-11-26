"""阶段三 A0：第一阶段 C 开发往返的离线稳态摩擦预筛。"""
import csv
import hashlib
import json
from collections import defaultdict
from pathlib import Path

import numpy as np
from reportlab.graphics import renderSVG
from reportlab.graphics.charts.lineplots import LinePlot
from reportlab.graphics.shapes import Drawing, String
from reportlab.graphics.widgets.markers import makeMarker
from reportlab.lib import colors


ROOT = Path(__file__).resolve().parents[1]
POSITION = ROOT.parent / "01_位置相关空载电流辨识"
INERTIA = ROOT.parent / "02_等效惯性电流辨识"
SESSION = "Z-C_20261124-165139_02"
TAG = SESSION + ".stage3_A0"
MODEL = POSITION / "models/position_B_primary_v1.json"
ALPHA = INERTIA / "results/b/Z-BP_20261125-155725_864_47920_summary.json"
MANIFEST = POSITION / "manifest"
RAW = POSITION / "data/raw" / (SESSION + ".csv")
SPEEDS = (2000, 10000, 40000)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_csv(path):
    with path.open(encoding="utf-8-sig", newline="") as f:
        return list(csv.DictReader(f))


def write_csv(path, rows):
    with path.open("w", encoding="utf-8-sig", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def longest_plateau(rows, trial, policy, meta, model):
    scan = [r for r in rows if r["stage"] == "scan"]
    require(len(scan) >= 3, "缺少扫描段")
    t, x, current = np.array([[float(r[k]) for k in ("t_s", "x_mm", "current_fb_raw")]
                              for r in scan]).T
    require(np.isfinite([t, x, current]).all() and np.all(np.diff(t) > 0), "采样值或时钟无效")
    scale = float(meta["pulse_per_mm"])
    x *= scale
    direction = int(trial["direction"])
    speed = scale * float(trial["speed_mm_s"])
    # 左右各至少 span；两端不能用不完整窗口。按第一阶段相同的割线公式估计 v、a。
    span = policy["scanDerivativeSpanS"]
    left = np.searchsorted(t, t - span, side="right") - 1
    right = np.searchsorted(t, t + span)
    idx = np.flatnonzero((left >= 0) & (right < len(t)))
    l, h = left[idx], right[idx]
    before = (x[idx] - x[l]) / (t[idx] - t[l])
    after = (x[h] - x[idx]) / (t[h] - t[idx])
    v = (x[h] - x[l]) / (t[h] - t[l])
    a = 2 * (after - before) / (t[h] - t[l])
    gaps = np.r_[0, np.cumsum(np.diff(t) > float(trial["period_s"]) * policy["gapPeriods"])]
    state_ok = np.array([int(r["status"]) & 0x400 != 0 and r["enabled"] == "1"
                         and r["mode"] == meta["position_mode"] and
                         all(r[k] == "0" for k in ("alarm", "limit_pos", "limit_neg"))
                         for r in scan])
    good = ((gaps[l] == gaps[h]) & state_ok[idx]
            & (np.maximum(abs(direction * before - speed), abs(direction * after - speed))
               <= policy["speedFraction"] * speed)
            & (abs(a) <= scale * max(policy["accelFloor"], float(meta["acc_mm_s2"]) * policy["accelFraction"])))
    kept = np.flatnonzero(good)
    cuts = np.flatnonzero((np.diff(idx[kept]) != 1) | (direction * np.diff(x[idx[kept]]) <= 0)) + 1
    runs = [run for run in np.split(kept, cuts) if len(run) >= 3]
    require(runs, "无连续实测匀速段")
    run = max(runs, key=lambda r: abs(x[idx[r[-1]]] - x[idx[r[0]]]))
    if direction < 0:
        run = run[::-1]
    sample = idx[run]
    lo, hi = model["range_pulse"]
    inside = (lo <= x[sample]) & (x[sample] <= hi)
    run, sample = run[inside], sample[inside]
    require(len(sample) >= 3, "匀速段与位置表缺少重合")
    residual = current[sample] - np.interp(x[sample], model["node_pulse"], model["current_raw"])
    return x[sample], v[run], a[run], residual


def plot(path, pairs):
    drawing = Drawing(590, 325)
    chart = LinePlot()
    chart.x, chart.y, chart.width, chart.height = 67, 52, 491, 222
    chart.data = []
    for direction, color in (("negative", "#2362A0"), ("positive", "#B04A24")):
        for group in (1, 2, 3):
            points = [(p[direction + "_v_pulse_s"] / 1000, p[direction + "_raw"])
                      for p in pairs if p["group"] == group]
            chart.data.append(sorted(points))
            line = chart.lines[len(chart.data) - 1]
            line.strokeColor = colors.HexColor(color)
            line.strokeWidth = 0.6
            line.symbol = makeMarker("FilledCircle", size=4, fillColor=line.strokeColor)
    chart.xValueAxis.valueMin, chart.xValueAxis.valueMax = -45, 45
    chart.yValueAxis.visibleGrid = True
    chart.yValueAxis.gridStrokeColor = colors.HexColor("#DDDDDD")
    chart.xValueAxis.joinAxisMode = "bottom"
    chart.yValueAxis.joinAxisMode = "left"
    drawing.add(chart)
    drawing.add(String(67, 306, "Z-axis A0 | Steady sliding current by direction", fontSize=15))
    drawing.add(String(67, 285, "Feedback minus position term (raw)   |   3 development round trips per speed", fontSize=10))
    drawing.add(String(160, 18, "Measured encoder velocity (1,000 pulses/s)", fontSize=11))
    renderSVG.drawToFile(drawing, str(path))


def main():
    model = json.loads(MODEL.read_text(encoding="utf-8"))
    inertia = json.loads(ALPHA.read_text(encoding="utf-8"))
    meta_file = MANIFEST / (SESSION + ".txt")
    meta = dict(line.split("=", 1) for line in meta_file.read_text(encoding="utf-8-sig").splitlines() if "=" in line)
    require(model["role"] == "B_primary" and model["c_session"] == SESSION
            and model["raw_sha256"][SESSION] == digest(RAW), "模型来源或原始数据哈希不符")
    require(meta["outcome"] == "completed" and all(meta[k] == v for k, v in model["conditions"].items()),
            "会话状态或信号配置不匹配")
    plan_file = MANIFEST / (SESSION + ".plan.csv")
    trial_file = MANIFEST / (SESSION + ".trials.csv")
    plans = {p["trial_id"]: p for p in read_csv(plan_file) if p["use"] == "development"}
    trials = [t for t in read_csv(trial_file) if t["trial_id"] in plans]
    samples = defaultdict(list)
    with RAW.open(encoding="utf-8-sig", newline="") as f:
        for r in csv.DictReader(f):
            if r["trial_id"] in plans:
                samples[r["trial_id"]].append(r)
    curves = {}
    ids = {}
    for trial in trials:
        tid = trial["trial_id"]
        plan = plans[tid]
        require(trial["status"] == "completed" and all(trial[k] == val for k, val in plan.items()),
                f"试次 {tid} 与计划不符")
        require(all(r["valid"] == "1" and all(r[k] == trial[k] for k in
                    ("session_id", "phase", "group", "use", "direction", "speed_mm_s"))
                    for r in samples[tid]), f"试次 {tid} 原始记录不符")
        speed = float(meta["pulse_per_mm"]) * float(trial["speed_mm_s"])
        key = (speed, int(trial["group"]), int(trial["direction"]))
        require(key not in curves, "重复试次")
        try:
            curves[key] = longest_plateau(samples[tid], trial, model["scan_policy"], meta, model)
        except ValueError as error:
            raise ValueError(f"试次 {tid}: {error}") from error
        ids[key] = tid
    expected = {(s, g, d) for s in SPEEDS for g in (1, 2, 3) for d in (-1, 1)}
    require(set(curves) == expected, "开发集缺少预定的完整往返")
    step = int(float(meta["pulse_per_mm"]) * model["scan_policy"]["gridMm"])
    lo = max(3500, max(c[0][0] for c in curves.values()))
    hi = min(22000, min(c[0][-1] for c in curves.values()))
    grid = np.arange(np.ceil(lo / step) * step, np.floor(hi / step) * step + 1, step)
    require(len(grid) >= 3, "共同位置不足三个网格点")
    require(inertia["status"] == "matrix_complete", "惯性候选结果无效")
    alpha_bound = max(abs(a) for a in inertia["fitZero"]["alphaCi95"])
    pairs, summary, profile = [], [], []
    for speed in SPEEDS:
        repeats = []
        for group in (1, 2, 3):
            p = dict(group=group, speed_pulse_s=speed, min_pulse=grid[0], max_pulse=grid[-1], nodes=len(grid))
            signals = []
            for d, name in ((1, "positive"), (-1, "negative")):
                key = (speed, group, d)
                x, v, a, y = curves[key]
                current = np.interp(grid, x, y)
                signals.append(current)
                p[name + "_trial"] = ids[key]
                p[name + "_v_pulse_s"] = np.interp(grid, x, v).mean()
                p[name + "_raw"] = current.mean()
                # 包括跨过两端网格点的插值支撑样本，保留未经空间插值的加速度峰值。
                start = max(0, x.searchsorted(grid[0]) - 1)
                end = min(len(x), x.searchsorted(grid[-1], side="right") + 1)
                p[name + "_inertia_sensitivity_max_raw"] = alpha_bound * abs(a[start:end]).max()
            pos, neg = signals
            for x, yp, yn in zip(grid, pos, neg):
                profile.append(dict(group=group, speed_pulse_s=speed, x_pulse=x,
                                    positive_raw=yp, negative_raw=yn, half_diff_raw=(yp-yn)/2, half_sum_raw=(yp+yn)/2))
            p["half_diff_raw"] = ((pos - neg) / 2).mean()
            p["half_sum_raw"] = ((pos + neg) / 2).mean()
            repeats.append(p)
        pairs.extend(repeats)
        row = dict(speed_pulse_s=speed, round_trips=len(repeats))
        for field in ("positive_raw", "negative_raw", "half_diff_raw", "half_sum_raw"):
            values = [p[field] for p in repeats]
            row[field + "_mean"] = np.mean(values)
            row[field + "_sd"] = np.std(values, ddof=1)
        row["inertia_sensitivity_max_raw"] = max(p[d + "_inertia_sensitivity_max_raw"]
                                                  for p in repeats for d in ("positive", "negative"))
        summary.append(row)
    result = ROOT / "results" / TAG
    figures = ROOT / "data/figures"
    result.mkdir(parents=True, exist_ok=True)
    figures.mkdir(parents=True, exist_ok=True)
    for name, rows in (("pairs", pairs), ("summary", summary), ("profiles", profile)):
        write_csv(result / (name + ".csv"), rows)
    plot(figures / (TAG + ".svg"), pairs)
    files = [RAW, MODEL, ALPHA, meta_file, plan_file, trial_file, Path(__file__).resolve()]
    provenance = dict(session=SESSION, use="development", current_unit="raw",
                      grid_pulse=[grid[0], grid[-1], step], policy=model["scan_policy"],
                      alpha_sensitivity_only=alpha_bound, alpha_unit="raw/(pulse/s^2)",
                      sha256={str(p.relative_to(ROOT.parent.parent)): digest(p) for p in files})
    (result / "sources.json").write_text(json.dumps(provenance, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"{len(curves)} scans, {len(pairs)} round trips, {len(grid)} common nodes. {result}")


if __name__ == "__main__":
    main()
