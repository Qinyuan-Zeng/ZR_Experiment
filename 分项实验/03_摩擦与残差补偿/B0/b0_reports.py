"""Build the B0 technical and decision notes from the audit tables."""
import numpy as np
import pandas as pd

from b0_sources import OUT, load


def load_csv(name):
    return pd.read_csv(OUT/name,low_memory=False)


def md(name, content):
    (OUT/name).write_text(content.strip()+"\n",encoding="utf-8")


def number(value,digits=2):
    return f"{value:.{digits}f}" if np.isfinite(value) else "无"


def rows_read(inv, cohort):
    df=inv[inv["cohort"].eq(cohort)&inv["raw_quality"]]
    if cohort in ("S1_C","S3_A_final"):
        df=df[df["dataset_role"].eq("train_candidate")]
    return df


def latency_note(inv,lag,metric):
    cohort_table=[]
    for cohort,label in (("S1_C","第一阶段 C 开发腿"),("S2_B_original","第二阶段原 B Watch"),
                         ("S2_B_expanded","第二阶段扩展 B Watch"),("S3_A_final","第三阶段 A 开发腿")):
        df=rows_read(inv,cohort)
        n=len(df)
        m=lambda col,d=3:number(df[col].median(),d) if df[col].notna().any() else "—"
        cohort_table.append(f"| {label} | {n} | {m('native_p50_ms')} | {m('native_p95_ms')} | "
                            f"{m('host_read_span_p95_ms') if cohort.startswith('S1') or cohort.startswith('S3') else '—'} | "
                            f"{m('grid_age_p95_ms')} | {m('command_bracket_ms')} |")
    offset_table=[]
    for cohort,q in lag.groupby("cohort",sort=True):
        runs=q.pivot(index="run_id",columns="shift_ms",values="centered_rmse_raw").dropna()
        zero=q[q["shift_ms"].eq(0)]
        offset_table.append(f"| {cohort} | {len(runs)} | {runs.median().idxmin():g} | "
                            f"{runs.idxmin(axis=1).eq(0).sum()}/{len(runs)} | {number(runs[0].median())} | "
                            f"{number(zero['ref_causal_a_rms_mm_s2'].median(),1)} |")
    dual=[]
    keep=metric[metric["scope"].eq("phase")&metric["dataset_role"].eq("train_candidate")]
    for cohort in ("S2_B_expanded","S3_A_final","S1_C"):
        for phase in ("acceleration","deceleration"):
            s=keep[keep["cohort"].eq(cohort)&keep["phase"].eq(phase)].set_index("metric")
            ra,ca="ref_r_M1_dual_common_raw","causal_r_M1_dual_common_raw"
            if ra in s.index and ca in s.index:
                assert s.loc[ra,"samples"]==s.loc[ca,"samples"]
                dual.append(f"| {cohort} | {phase} | {int(s.loc[ra,'runs'])} | "
                            f"{int(s.loc[ra,'samples'])} | {number(s.loc[ra,'run_balanced_rmse_raw'])} | "
                            f"{number(s.loc[ca,'run_balanced_rmse_raw'])} |")
    md("latency_and_causality.md",f"""
# B0 时间对齐与因果重建

## 时钟来源与限度

| 来源 | 腿数 | 原始间隔 P50 中位数 (ms) | 单腿间隔 P95 中位数 (ms) | 主机 SDK 读窗口 P95 中位数 (ms) | 5 ms 格点观测年龄 P95 中位数 (ms) | 指令前后卡侧间隔 P50 (ms) |
|---|---:|---:|---:|---:|---:|---:|
{chr(10).join(cohort_table)}

主机 CSV 的时间戳在顺序 SDK 调用之前。重放取 `t_s + read_s` 作为整帧最早可用时间，同一行编码器与电流的真实捕获相位在 SDK 读窗口内仍不可分辨。Watch 的编码器和电流共享卡侧中断 tick，使用冻结核周期和 32 位回绕校正，指令前后 tick 满足日志上的时间包络。帧内物理相位和驱动器电流滤波延迟没有独立时间戳，现有数据无法拆出这两项。Watch 状态来自分开的 SDK 轮询。 无逐腿控制器增益快照的记录，复用判断保留该证据缺口。

## 统一与模型口径

- 统一整数概念位置 pulse、速度 pulse/s、加速度 pulse/s²、电流 raw。原始主机 `x_mm` 按 1,000 pulse/mm 转换，Watch 编码器列直接使用 pulse。运动和电流的实际观测源只来自反馈。
- 以每腿首个原始采样时刻为相对零点，生成 5 ms 查询网格。格点使用最近且已可用的原始电流和编码器观测，`t_native_s` 保存原始相对时刻，`t_available_s` 记录因果可用时间。相邻原始时刻倒退、卡帧重复、质量位异常或间隔超过 2.5 个来源请求周期时切断滤波窗口。
- 离线参考在同一原始电流时刻用 ±10 ms 的五点中心二次拟合，对位置插值后取速度及加速度。因果重放用五个过去原始位置，按真实时间做一侧二次拟合。Watch 选过去相隔约 5 ms 的原始帧，主机选最近五个不同原始点。两法在不同边界各自留下有效掩码。5 ms 输出格点是兼容性网格，主机最近原始观测可能重复，Watch 的 50 µs 帧率只用于定位源帧。
- 因果历史量以首个有效速度初始化，速度指数时间常数 50 ms，连续静止时间在实际停稳后递增。无有效状态或记录分段、重启时重新初始化并保留历史年龄。阶段标签用于审计和误差分层。预备、到位保持来自该时刻可用的状态和记录事件，叠加编码器停稳门槛才令零速摩擦项为零。滑动近零区仍无有效 F1 标签。
- 位置查表限 1,000～31,000 pulse。M1 基础滑动支持限 10,000～22,000 pulse 和 2,000～40,000 pulse/s；区间外仍保留单独命名的空间外推诊断列。超过 40,000 或未验证的近零滑动无 F1 数值。惯性项使用冻结 α，按重建加速度标注 `near_steady`、`below_400k`、`400k_to_600k`、`above_600k`。该连续 αa 是诊断基线，区间外未承诺校准精度。

## 延迟敏感性

| 来源 | 通过可辨性筛选的腿数 | 最佳群体偏移 (ms) | 逐腿最优恰为零 | 零偏移中心化 RMSE 中位数 (raw) | 离线与因果加速度差 RMS 中位数 (mm/s²) |
|---|---:|---:|---:|---:|---:|
{chr(10).join(offset_table)}

敏感性以固定电流观测、固定位置项和 F1 为基线，比较加速及减速行的 αa 在 −20～+20 ms 范围内的九个平移。所有偏移用完全相同的支持点，每腿先去除常值偏差，再比较中心化误差。正偏移表示使用相对电流更晚的加速度，结果只用于离线诊断。每腿需至少八个比较点且 αa 有足够变化，较短或退化的主机动态片段因此不能给出可信偏移。本次保留原始零平移用于全部输出，未从误差极小点推导控制器物理延迟，也未重拟合 α。 第二阶段 C 的已有积分校核仍提示约 25%～29% 的口径差异。B0 的平移敏感性只检验本重建下的时间位移，尚不足以排除带宽、窗函数或积分边界造成的差异。 [逐腿偏移表](lag_sensitivity.csv) · [偏移图](figures/03_latency_sensitivity.svg)

| 来源 | 阶段 | 腿数 | 共同样本 | 离线 M1 等腿权 RMSE (raw) | 因果 M1 等腿权 RMSE (raw) |
|---|---|---:|---:|---:|---:|
{chr(10).join(dual)}

上表只使用两种重建均有 M1 支持的同一批观测，避免因为掩码不同造成比较偏差。5 ms 是本次离线审计网格。后续推理的刷新周期、抖动和状态估计延迟需在 B1 单独测定。 [残差分层表](mechanism_residuals.csv) · [Watch 代表轨迹](figures/02_watch_time_trace.svg)
""")


def summary(inv, gap, metric, repeats, provenance):
    files=load_csv("source_files.csv")
    aliases=load_csv("source_aliases.csv")
    train=inv[inv["dataset_role"].eq("train_candidate")]
    enough=train[train["usable_for_model_development"].eq(True)]
    hold=inv[inv["dataset_role"].eq("hold_candidate")]
    sources=[]
    for cohort,name,meaning in (
        ("S1_C","第一阶段 C","开发慢采样完整扫描，原位置评价留出腿仅作诊断。"),
        ("S2_B_original","第二阶段原 B","Watch 动态开发，只含正向真实运动。"),
        ("S2_B_expanded","第二阶段扩展 B","Watch 动态开发，只含正向真实运动；两腿是同向加速／减速匹配。"),
        ("S3_A_final","第三阶段 A","F1 开发腿可用于 B0 开发；留出和第五轮分别标记为既有诊断。"),
    ):
        hist=train[train["cohort"].eq(cohort)]
        usable=enough[enough["cohort"].eq(cohort)]
        sources.append(f"| {name} | {len(hist)} | {hist['pair_id'].nunique()} | {len(usable)} | "
                       f"{int(usable['usable_causal_dynamic'].sum())} | {meaning} |")
    audit=[]
    for k in ("B1","B2","bridge"):
        t=gap[gap["stage"].eq(k)]
        audit.append(f"| {k} | {len(t)} | {int(t['planned_pairs'].sum())} | "
                     f"{int(t['accepted_substitute_pairs'].sum())} | {int(t['pairs_to_acquire'].sum())} | "
                     f"{t['A_protection_screen'].eq('fails').sum()}/{len(t)} |")
    s=metric[(metric["scope"]=="phase")&(metric["dataset_role"]=="train_candidate")]
    model_rows=[]
    for cohort in ("S2_B_expanded","S3_A_final"):
        for phase in ("acceleration","steady","deceleration","hold"):
            z=s[s["cohort"].eq(cohort)&s["phase"].eq(phase)].set_index("metric")
            if "ref_r_M0_common_raw" not in z.index: continue
            mm=z.loc["ref_r_M1_common_raw"]
            m0=z.loc["ref_r_M0_common_raw"]
            assert m0["samples"]==mm["samples"]
            model_rows.append(f"| {cohort} | {phase} | {int(mm['runs'])} | {number(m0['run_balanced_rmse_raw'])} | "
                              f"{number(mm['run_balanced_rmse_raw'])} | {number(mm['mae_raw'])} | {number(mm['p95_abs_raw'])} |")
    valid_reps=repeats[repeats["trace_pair_count"].gt(0)]
    rep_rows=[]
    for (cohort,phase),q in valid_reps.groupby(["cohort","phase"]):
        rep_rows.append(f"| {cohort} | {phase} | {len(q)} | {number(q['trace_pearson_r_median'].median())} | "
                        f"{number(q['between_run_bias_sd_raw'].median())} | "
                        f"{number(q['between_session_pearson_r_median'].median())} |")
    short=gap[(gap["stage"]=="B1")&gap["ramp_s"].lt(.015)]
    b1=gap[gap["stage"].eq("B1")]
    reverse=int(b1["partial_joint_state_negative_grid"].sum())
    if reverse==0:
        direction_note="32 格的实测联合搜索中，合格反向状态格点均为 0。既有 A/C 有反向扫描，但它们的主要加减速位置在 P1/P2 之外；第二阶段 B Watch 则只有正向。"
    else:
        direction_note=f"B1 联合状态搜索的反向格点计数合计为 {reverse}。该合计允许同一个样本落入多个计划格，不能用于独立重复数。"
    md("B0_summary.md",f"""
# 第三阶段 B0 历史复用与动态残差审计

**B0 结论。** 历史数据可支撑 NN 输入、时序和小规模原型的开发，也有可重复残差的证据。实际风险集中在反向动态、两条短路径的完整起停、低速短加速段和跨时段复现。它们尚不能支撑覆盖整个 2～40 mm/s 的正式第一版动态 NN 训练与内部定型。本次按预先定义的等价规则，**B1 可直接抵扣 0 对，B2 可直接抵扣 0 对**。本次仅作离线审计。

本次盘点 **{len(inv):,} 条原始试次或计划占位**，登记 **{len(files):,} 个来源文件**，导出 **{provenance['five_ms_samples']:,} 个带掩码的 5 ms 格点**。冻结机理口径为位置表 `position_B_primary_v1`、α={provenance['alpha_raw_per_pulse_s2']:.16g} raw/(pulse/s²)、正式 A `Z-AF-20261126-03` 的 F1。阶段二记录的表内容 SHA-256 和阶段三记录的整个 JSON 文件 SHA-256 分别核验，指向同一冻结位置表。 [清单](run_inventory.csv) · [来源哈希](source_files.csv) · [冻结版本](audit_provenance.json)

## 历史数据能做什么

| 来源 | 开发候选腿 | 对应完整往返或原匹配组 | 具备至少 3 个因果动态有效格点的腿 | 这些腿的因果动态格点 | 用途 |
|---|---:|---:|---:|---:|---|
{chr(10).join(sources)}

另有第一阶段主 B 的 **{len(hold)}** 次位置保持候选，只供 HOLD 和位置误差诊断。全体历史样本的 E 资格均为 **0**。第二阶段 C 引用的 **{int(aliases["analysis"].eq("S2_C_integral").sum())}** 条原始腿以别名登记，新增独立重复为 0。静态、较早方法探索、质量不合格和已有模型留出数据分别保留历史角色。同一往返、相邻窗口以及同一历史会话归为同一候选组，缺少可信现场会话边界时按来源日期保守分组。尚未把这些历史候选切成训练集和内部验证集，也未形成正式 E。 [去重记录](source_aliases.csv) · [状态覆盖](state_coverage.csv)

三点阈值仅用于量化基本可见性，连续时长和可辨性仍由每个工况的原始时间轴决定。主机 C/A 原始间隔的单腿中位数约 6 ms。第二阶段 Watch 正向记录提供更多动态样本，仍需补到相同位置、速度和加速度下的方向对照。{direction_note} [时序诊断](latency_and_causality.md) · [联合状态图](figures/01_B1_state_coverage.svg)

## 机理残差和重复性

| 来源 | 阶段 | 同支持腿数 | M0 等腿权 RMSE (raw) | M1 等腿权 RMSE (raw) | M1 MAE (raw) | M1 绝对误差 P95 (raw) |
|---|---|---:|---:|---:|---:|---:|
{chr(10).join(model_rows)}

| 来源 | 阶段 | 具备形状重叠的条件数 | 同条件残差曲线相关系数中位数 | 腿间平均偏差标准差中位数 (raw) | 现场会话均值曲线相关系数中位数 |
|---|---|---:|---:|---:|---:|
{chr(10).join(rep_rows)}

共同支持的 M0/M1 指标使用同一批观测。等腿权 RMSE 先在每腿取均方，再在腿间等权平均后开根。重复性按工况、方向和阶段统计，形状相关至少具有四个共用的 1 mm 位置箱；满足条件的样本偏向有足够空间跨度的稳态或长动态段。稳态残差形状可复现，部分 Watch 动态也有重复结构。动态相关在不同阶段和来源上并不一致，尤其主机端短加减速段与跨时段对照证据偏少。先在 B1 固定条件做重复对照，才能检验这些结构能否泛化。工程允许的 raw 误差门槛尚缺受控力误差预算和 B1 动态重复证据，留待正式验收前冻结。 [分层残差](mechanism_residuals.csv) · [重复性](repeated_conditions.csv) · [共同支持图](figures/04_common_support_errors.svg)

短时保持需单独建模。停稳掩码内 F1 设为零，因此表中的 HOLD 行上 M0 和 M1 误差相同。Watch 的停稳后记录约为 0.15 s，正式 A 的换向观察也较短。这些统计体现起停历史下的短时偏差，不能直接当作长保持误差。B1 先保留运动前后约 1 s 的波形，为 C/D 零速衔接和停驻历史分析提供证据。

## B1 与 B2 缺口及调整

| 计划 | 候选工况 | 计划往返 | 历史直接抵扣 | 仍需往返 | 沿用 A 保护条件时停车预算失败工况 |
|---|---:|---:|---:|---:|---:|
{chr(10).join(audit)}

**B1 保留 32 个工况和跨时段 3＋2 的目标，分批进入。** 首批优先在 P1/P2 上跑 10/400、20/400、20/600、40/400、40/600（速度 mm/s／加速度 mm/s²）各一整对，共 10 对。重点检查反向加减速、运动前后各约 1 秒保持、卡侧时间戳和主机读数对齐。通过同一冻结验收规则的首批可计入目标。随后覆盖剩余合法工况。2/5 mm/s 的 {len(short)} 个路径工况依旧承担起停、低速与保持覆盖，理论加速只需 5/12.5 ms；若继续用主机约 6 ms 采样，不能把这些工况作为分离不同加速度的证据。必要时用同步 Watch 记录来验证原始动态分辨率。

**B2 保留 60→80→100 的目标，原 H1/H2 点表待重算。** 首先用 B1 校准同步和可观测性，再在经现场验收的新路径上验证 60 mm/s。按 A 的保护预算，12 个高速工况的低端停车空间均不足。仅作同口径预算，在 0～37 mm 内，100 mm/s 两端各需约 9.56 mm，最大对称行程约 17.88 mm。500 mm/s² 至少需 20 mm 才能达到目标，因此这格在该预算下无可行区间。600 mm/s² 的理论恒速段也很短，只能在有充分实测证据时用于动态扩展。可选桥接先保留 40 mm/s 长路径、60 mm/s 短路径的归因目的，分别预检后定稿。E 继续在训练前预注册与冻结，预留记录不得用于时序或模型参数选择。 [逐工况缺口](gap_matrix.csv)

## 文件和复现

- [逐时刻机理电流与残差](mechanism_samples.csv.gz) 为压缩 CSV，按行保存原始观测年龄、两套状态、各物理项、M0/M1 预测、误差和掩码。用 `run_id` 回查 [原始试次盘点](run_inventory.csv)，以 `pair_id` 统计独立往返或历史同向匹配对。
- [可复现审计](b0_audit.py)、[状态重建](b0_states.py)、[来源适配](b0_sources.py)、[绘图](b0_figures.py) 和 [报告生成](b0_reports.py) 均只读取已有证据。使用工作区的 Python 3.12、NumPy、pandas、ReportLab 和 Node.js 顺序运行 `b0_audit.py`、`b0_figures.py`、`b0_reports.py`，再运行 `b0_verify.py`。所有输出位于此目录。

从工作区根目录运行以下 PowerShell 命令。Python 环境须提供 NumPy、pandas 和 ReportLab，`node` 须可在 PATH 中找到。

```powershell
$b0 = './分项实验/03_摩擦与残差补偿/B0'
$py = 'C:/Users/HYB/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe'
$env:PYTHONDONTWRITEBYTECODE = '1'
foreach ($task in 'b0_audit.py','b0_figures.py','b0_reports.py','b0_verify.py') {{
    & $py (Join-Path $b0 $task)
    if ($LASTEXITCODE -ne 0) {{ break }}
}}
```

""")


def main():
    inv=load_csv("run_inventory.csv")
    metric=load_csv("mechanism_residuals.csv")
    latency_note(inv,load_csv("lag_sensitivity.csv"),metric)
    summary(inv,load_csv("gap_matrix.csv"),metric,load_csv("repeated_conditions.csv"),load(OUT/"audit_provenance.json"))
    print("B0_summary.md and latency_and_causality.md written")


if __name__=="__main__":
    main()
