"""Rebuild Markdown reports and representative waveforms from sealed outputs."""
from html import escape
from common import *


def frame(name):
    return pd.read_csv(RESULTS/f"{name}.csv")


def md(name, text):
    (HERE/name).write_text(text.strip()+"\n",encoding="utf8")


def table(data):
    cols = list(data.columns)
    values = lambda row: [f"{v:.4g}" if isinstance(v,(float,np.floating)) else str(v) for v in row]
    lines = [cols,["---"]*len(cols),*(values(row) for row in data.itertuples(index=False,name=None))]
    return "\n".join("| "+" | ".join(map(str,line))+" |" for line in lines)


def error_summary(df, scope="ALL"):
    phases = ["ALL","loading","accelerate","cruise","brake","hold","hold_low_speed"]
    df=df[df.cohort.eq(scope)&df.phase.isin(phases)]
    return df.groupby(["model","phase"]).rmse.mean().unstack()[phases].reset_index()


def svg(name, title, time, panels):
    palette = ["#263238","#1768a8","#d4522a","#488347","#7e47a1","#a17d21"]
    left,right,w = 100,900,960
    height = 62+193*len(panels)
    body = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{height}" viewBox="0 0 {w} {height}">',
            '<rect width="100%" height="100%" fill="#ffffff"/>',
            f'<text x="30" y="29" font-size="18" font-weight="600">{escape(title)}</text>',
            '<g font-family="Arial, Microsoft YaHei, sans-serif" font-size="12">']
    t0,t1=float(min(time)),float(max(time))
    for j,(unit,data) in enumerate(panels):
        top, bottom = 62+j*193, 202+j*193
        series = [(label,np.asarray(y,float)) for label,y in data]
        ranges = np.concatenate([y[np.isfinite(y)] for _,y in series])
        lo,hi = float(ranges.min()),float(ranges.max())
        bias = max(.1,(hi-lo)*.06)
        lo,hi=lo-bias,hi+bias
        for f in np.linspace(0,1,5):
            y=bottom-f*(bottom-top)
            x=left+f*(right-left)
            body.append(f'<line x1="{left}" x2="{right}" y1="{y}" y2="{y}" stroke="#e2e7e9"/>')
            body.append(f'<line x1="{x}" x2="{x}" y1="{top}" y2="{bottom}" stroke="#e2e7e9"/>')
            body.append(f'<text x="{left-7}" y="{y+4}" fill="#425466" text-anchor="end">{lo+f*(hi-lo):.4g}</text>')
            body.append(f'<text x="{x}" y="{bottom+16}" text-anchor="middle" fill="#425466">{t0+f*(t1-t0):.4g}</text>')
        body.append(f'<text x="{left}" y="{top-9}" fill="#283645" font-weight="bold">{escape(unit)}</text>')
        body.append(f'<text x="{right+6}" y="{bottom+16}" fill="#425466">s</text>')
        for i,(label,y) in enumerate(series):
            xplot=left+(np.asarray(time)-t0)/(t1-t0)*(right-left)
            yplot=bottom-(y-lo)/(hi-lo)*(bottom-top)
            mask = np.isfinite(y)
            col=palette[i]
            for start,end in valid_runs(mask):
                pts=" ".join(f"{x:.2f},{z:.2f}" for x,z in zip(xplot[start:end],yplot[start:end]))
                body.append(f'<polyline points="{pts}" fill="none" stroke="{col}" stroke-width="1.6"/>')
            xleg=left+155*i
            body.append(f'<line x1="{xleg}" x2="{xleg+18}" y1="{bottom+33}" y2="{bottom+33}" stroke="{col}" stroke-width="2"/>')
            body.append(f'<text x="{xleg+23}" y="{bottom+37}" fill="#263238">{escape(label)}</text>')
    body.extend(["</g>","</svg>"])
    (RESULTS/name).write_text("\n".join(body),encoding="utf8")


def valid_runs(mask):
    edge=np.flatnonzero(np.diff(np.r_[False,mask,False]))
    return zip(edge[::2],edge[1::2])


def plots(seal):
    scored=pd.read_csv(RESULTS/"C_internal_internal_predictions.csv.gz")
    ref=pd.read_pickle(RESULTS/"native_reference.pkl")
    focus="B1:R5-P2-V40-A600_leg1"
    br=scored[scored.trace.eq(focus)&scored.frame.between(23800,28300)]
    br=br.merge(ref,on=["trace","frame"],validate="one_to_one")
    k=br.loc[br.frame.eq(26200)].iloc[0]
    current = lambda d: [("I feedback",d.current_raw),("M1 S0",d.m1_raw),("M1 revised",d.m1_alt),
                         ("T0",d.T0_426),("T1",d.T1_426)]
    svg("brake_reversal.svg",focus+" | t=0 at frame 26200",(br.frame-26200)*prep.TICK,
        [("Position [pulse]",[("Encoder",br.x_pulse)]),
         ("Velocity [pulse/s]",[("Watch proxy",br.ref_v),("S0",br.v_pulse_s),(seal["state"],br.v_alt)]),
         ("Acceleration [pulse/s²]",[("Watch proxy",br.ref_a),("S0",br.a_pulse_s2),(seal["state"],br.a_alt)]),
         ("Current [raw] (transition shown for diagnosis)",current(br))])
    h=scored[scored.unit.eq("B1:R5-P1-V2-A400")&scored.phase.eq("hold")]
    trace=h.groupby("trace").size().idxmax()
    h=h[h.trace.eq(trace)]
    svg("low_speed_hold.svg",trace+" | HOLD after 2 mm/s arrival",h.hold_age_s,
        [("Current [raw]",[("I feedback",h.current_raw),("T0",h.T0_426),("T1",h.T1_426),
                           ("T2",h.T2_426),("T3",h.T3_426)]),
         ("Error [raw] = measured - predicted",[(m,h.current_raw-h[f"{m}_426"]) for m in ("T0","T1","T2","T3")])])
    d=scored[scored.cohort.eq("D")&scored.phase.eq("hold")]
    trace=d.groupby("trace").size().idxmax()
    d=d[d.trace.eq(trace)]
    svg("long_hold.svg",trace+" | long HOLD",d.hold_age_s,
        [("Current [raw]",[("I feedback",d.current_raw),("T0",d.T0_426),("T2",d.T2_426),("T3",d.T3_426)])])
    return k


def main():
    from state import load_grid
    from memory_check import check
    import platform
    import sys
    import torch
    save("runtime.json",dict(python=sys.version,numpy=np.__version__,pandas=pd.__version__,
                             torch=torch.__version__,platform=platform.platform(),
                             threads=torch.get_num_threads()))
    seal=read(RESULTS/"selection_seal.json")
    a,b=read(RESULTS/"A_choice.json"),read(RESULTS/"B_choice.json")
    v=read(RESULTS/"baseline_verification.json")
    k=plots(seal)
    sdk=frame("A_S1_sdk_audit")
    sd=(sdk.groupby("cohort").agg(traces=("trace","size"),rows=("rows","sum"),
                                 return_code_ok=("rc_ok","sum"),interval_ms=("interval_ms_median","median"),
                                 p95_interval_ms=("interval_ms_p95","median"),
                                 multiples_20_pulse_ms=("multiples_20_pulse_ms","sum")).reset_index())
    counts=[]
    for label in ("dev","internal"):
        ev=frame(f"A_{label}_sign_events")
        x=ev.groupby("model").agg(events=("trace","size"),traces=("trace","nunique"),
                                  median_ms=("duration_ms","median"),duration_max_ms=("duration_ms","max"),
                                  peak_abs_pulse_s=("peak_v_pulse_s",lambda v:v.abs().max()),
                                  original_label_events=("original_label","sum")).reset_index()
        x.insert(0,"set",label)
        counts.append(x)
    md("A_速度反号与状态估计.md",f"""
# A 速度反号与状态估计

开发选择 **{a['chosen']}**。改进型首位为 **{a['exploratory']}**，供联合消融使用。S0 与 S2 在开发侧的固定机理加载误差几乎相同。S2 主要修正短时符号和静止波动。表现最好的 S3 候选仍推高加载误差，减速误差超过 S0 的两倍。S3 各组合的结果见同点覆盖和机制域交集表。

## S1 数据可用性

464 条原始 Watch 均只有帧号、卡 tick、编码器位置、反馈电流。SDK 旁路通过 `GTN_GetAxisEncVel` 记录反馈速度和返回码，但每次主机读取跨度内包含多个 API 请求，记录没有专属于反馈速度的卡时间戳。下表周期是读取间隔，不能由此推出控制器速度更新周期或其内部滤波。记录中这些速度多见 20 pulse/ms 的台阶，明显粗于低速试验的 2–5 pulse/ms。无法构造同步、可辨的 S1，故标记不可用。

{table(sd)}

## 减速末端复现与全记录扫描

B1 R5-P2-V40-A600 正向腿帧 26200 位于 {k.x_pulse:.0f} pulse，原始 Watch 前后各 5 ms 位移分别为 {k.dx_back:.0f} 和 {k.dx_ahead:.0f} pulse。S0 算得 {k.v_pulse_s:.1f} pulse/s，S2 为 {k.v_alt:.1f} pulse/s。该点反馈 {k.current_raw:.0f} raw，滑动摩擦由 {k.if_raw:.2f} 变为 {k.if_alt:.2f} raw，冻结 M1 由 {k.m1_raw:.2f} 变为 {k.m1_alt:.2f} raw。该点保留在原评价掩码。

下表以连续候选格点计一次事件，由原始位置居中速度及前后净位移共同判据识别。停尾候选还需要前 25 ms 存在明确行进方向。对低于阈值、位移信号先后矛盾的短暂反弹仅保留原始波形。换向另查 C 的接近与探测是否有相反的实测位移，确认范围是已采到的有停顿换向。没有连续运动直接换向标签。S2 将开发候选事件从 289 次降为 1 次，内部诊断从 205 次降为 0 次。S0 和 S2 都识别了开发和内部的 36 与 18 次可确认换向。

{table(pd.concat(counts))}

[SDK 核查](results/A_S1_sdk_audit.csv) · [开发反号事件](results/A_dev_sign_events.csv) · [内部反号事件](results/A_internal_sign_events.csv) · [制动波形](results/brake_reversal.svg)

## 冻结机理结果

{table(error_summary(frame('A_dev_metrics')))}

{table(frame('A_selection')[['model','loading','brake','hold','sign_events','extra_p95_absolute_delay_ms','true_reversal_recall','passes']])}

静止位置方差的训练侧第 90 百分位给出两折 R 基数 {read(RESULTS/'A_dev_R.json')['1']:.3f}、{read(RESULTS/'A_dev_R.json')['2']:.3f} pulse²。整数编码器的量化方差下限为 1/12 pulse²。静止测量中的位置波动也可能含真实伺服运动，因此采用该基数及其四倍作保守搜索。S3 只以位置测量更新；位置项仍查原编码器位置，变化仅沿惯性、滑动摩擦和后续残差传播。

对未通过主门槛的状态估计保留原辨识系数。新速度和加速度分布与原惯性标定域及 F1 支持域的重叠见覆盖表。退出滑动项的回退点仍计入固定掩码评分。机制域新增点只是候选支持范围，部分差异来自未套用原双窗口一致性规则。原数据未因此新增监督标签，另表按同一七点参考重做一致性敏感性。原静态 HOLD 置零沿用冻结回放条件，部署时的停驻进入判据仍需单独验收。

[分段运动状态](results/A_dev_kinematics.csv) · [响应延时及换向](results/A_dev_response.csv) · [电流分项与计算开销](results/A_dev_components.csv) · [S0 导数计算开销](results/A_S0_derivative_runtime.csv) · [原掩码](results/A_dev_metrics.csv) · [严格交集](results/A_dev_strict_metrics.csv) · [增减覆盖](results/A_dev_coverage.csv) · [内部状态诊断](results/A_internal_metrics.csv) · [七点一致性覆盖](results/A_aligned_support.csv) · [真实换向波形](results/real_reversal.svg)
""")
    grid=load_grid()
    memory=check(grid)
    from supplement import b_details, motion_waveforms, aligned_support
    b_details(grid)
    motion_waveforms(grid)
    aligned_support(grid)
    mem=memory.groupby(["model","hold_bin"]).mean_abs_delta_raw.mean().unstack().reset_index()
    bc=frame("B_cost").groupby("model").agg(parameters=("parameters","first"),train_s=("train_s","median"),
                                          infer_us=("online_us_per_grid","median"),updates=("updates","median")).reset_index()
    md("B_MLP与LSTM对照.md",f"""
# B MLP 与 LSTM 时序残差

全部组合使用 S0。首批比较的是 L1 与 L2 各自的 0.4 s 固定窗口和持续递推方式。L2 的当前输入取 shared_arrival 中六个显式历史特征，当前 x、v、a 已在序列末端。LSTM 与原 MLP 使用训练侧加权归一化、25 raw Huber、Adam 和同一组试次权重。窗口式按权重抽末端监督样本，流式每轮遍历完整试次并分块截断梯度，记录实际优化步数。

**开发选型为 {'沿用 MLP-V2' if b['chosen'] is None else b['chosen']}。** 8、32 单元和 0.1、1 s 按预设条件扩展，候选列表以开发表实际训练记录为准。

## 开发 RMSE

{table(error_summary(frame('B_metrics')))}

{table(frame('B_selection')[['model','loading','hold','accelerate','brake','hold_low_speed','passing_pairs','pass_error']])}

## 开销

{table(bc)}

训练时间为本机 CPU 中位数，推理为单点调用中位数。数值包含 Python 与窗口打包，硬件目标周期仍需独立测量。模型输入来自 5 ms 编码器位置流的因果特征。

## 流式记忆检验

在开发折 D 的完整记录上比较正常递推输出与诊断性 HOLD 起点置零隐藏状态输出。下表为各保持年龄的平均绝对残差输出变化（raw）。训练、正式评价和正常推理均贯通整个试次。差异随时间变小意味着当前网络逐渐忘记到位历史。该检验只测循环状态的贡献，L2 显式历史在两个回放中保持相同。L1 的长时复位响应仍明显，说明训练后存在持久的状态依赖。L1 的 HOLD 电流误差同时大幅上升，因此持久状态依赖并不等于记住了对电流预测有用的到位信息。L2 的循环状态响应在约 1 s 后已极小。保持阶段的显式历史特征此时仍可用，因此 L2 的保持表现不能作为循环状态保留多秒历史的证据。

{table(mem)}

[分阶段与 HOLD 窗](results/B_metrics.csv) · [三种子及计算开销](results/B_cost.csv) · [记忆复位诊断](results/B_D_hold_hidden_memory.csv) · [逐试次](results/B_by_trial.csv) · [工况](results/B_conditions.csv)
""")
    ct=frame("C_dev_metrics")
    cf=frame("C_internal_metrics")
    core=ct[ct.cohort.eq("ALL")&ct.phase.isin(["ALL","loading","accelerate","cruise","brake","hold","hold_low_speed"])].pivot(
        index=["fold","seed","phase"],columns="model",values="rmse")
    core["state_effect_at_MLP"]=core.T1-core.T0
    core["sequence_effect_at_S0"]=core.T2-core.T0
    core["interaction"]=core.T3-core.T1-core.T2+core.T0
    csv("C_development_contributions.csv",core.reset_index())
    rel=core.groupby("phase")[["state_effect_at_MLP","sequence_effect_at_S0","interaction"]].mean().reset_index()
    md("C_联合消融与最终建议.md",f"""
# C 联合消融与最终建议

**本轮推荐 {seal['preferred']}。** T0 为 S0＋MLP-V2，T1 仅换为 {seal['state']}，T2 仅换为 {'_'.join(map(str,seal['configs'][2][2]))}，T3 同时更换。所有组合在对应状态特征及对应物理残差上训练。若分支在 A 或 B 未达门槛，其联合对照仅用于解释贡献。误差以电流 raw 表示。

| 冻结回放检查 | 结果 |
| --- | ---: |
| 内部完整单位／有效点 | {v['units']}／{v['rows']} |
| M2 对原逐点预测最大差 | {v['M2_max_abs_point_delta']:.3g} raw |
| shared_arrival seed 426 对原逐点最大差 | {v['shared_arrival'][0]['max_abs_point_delta']:.3g} raw |
| shared_arrival 三种子各分段 RMSE 与原表最大差 | {max(n['max_abs_metric_rmse_delta'] for n in v['shared_arrival']):.3g} raw |

## 独立分折开发对照

{table(error_summary(ct))}

T1 加载相对 RMSE 下降约 1.55%，HOLD 下降约 2.25%，加载改善幅度低于 2% 门槛。T3 相对 T2 改善保持误差，和主要基准 T0 相比，加载与 HOLD 仍分别高约 3.82% 和 2.10%。T0 是保留基准，筛选门槛用于候选升级。

{table(frame('C_selection')[['model','loading','hold','brake','exploratory_branch','adoptable']])}

下表数值为同折同种子的 RMSE 差后取均值。交互项为 T3−T1−T2＋T0，负值表示这一指标下的额外改善。

{table(rel)}

## 既有内部留出，仅诊断

{table(error_summary(cf))}

同点评分使用原掩码，未对预测做时间平移，原有效点全部保留。另表给出对应状态方案共同支持域上的敏感性结果。未覆盖的起停、近零速只作波形诊断。内部结果不进入选择。E 保持独立，实际接触力精度待验证。

[开发交集](results/C_dev_strict_metrics.csv) · [开发工况](results/C_dev_conditions.csv) · [开发逐试次](results/C_dev_by_trial.csv) · [内部交集](results/C_internal_strict_metrics.csv) · [内部工况](results/C_internal_conditions.csv) · [内部逐试次](results/C_internal_by_trial.csv) · [内部波形数据](results/C_internal_internal_predictions.csv.gz) · [低速 HOLD](results/low_speed_hold.svg) · [长 HOLD](results/long_hold.svg)

## 最小补充实验

1. **制动与换向。** 优先补 P2 的 30、40 mm/s，两种制动时长、各两往返，共八往返。同步保留原生 Watch 和可用的在线位置／速度状态流。首次先核对速度读数自身时间戳、量化和更新规律。另在现有 C 往返路径上纳入一个可辨的连续换向诊断，供区分真实反向和数值反号。
2. **低速长保持。** 16 mm 处以 2、5 mm/s 正反向到位，各两次，保持 6 s，共八次。已有 D 的 10 mm/s 长 HOLD 可作为参照。这能独立检查到位方向记忆和幅值衰减，以及流式网络在 2–6 s 是否仍有收益。
3. **按状态估计器复核原机理。** 若延长制动后仍值得推进 S3，先用原惯性辨识实验的开发记录以该状态估计重新计算惯性候选，利用原恒速数据复核 F1。保持系数冻结和重辨识分别验收。补测用途及验收范围需另行冻结，与 E 独立。
""")
    print("Reports and waveform SVGs written",flush=True)

