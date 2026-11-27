# 第三阶段 D 停驻与 HOLD

D 研究空载轴停稳后电流相对冻结位置项的残差。比较到位方向、保持时间和位置，判断方向偏置是随时间衰减还是在稳定保持中仍可重复，以及多早可进入稳定 HOLD。保持残差为 `I_fb,raw − Ip(x_actual)`，静止时设惯性项与 F1 滑动项为零。减速至停稳之前的动态波形保留作衔接诊断。D 数据用于开发与诊断，E 的独立评价数据单独留存。

## 先复用，再补采

- 第一阶段 B 用于位置模型拟合，第一阶段 D1/D2 曾用于位置模型验证。两类记录可诊断较慢接近后的保持偏置。旧台账以首个有效 HOLD 样本起算，各来源的分析不混合。
- B1 使用验收后的逐腿停稳尾段。C 使用验收后的中心停留与探测后短尾，共用同一原始试次 ID。同步 Watch 采用卡内到位确认事件作时间零点。该事件在轴实际进入到位容差后产生，早于确认点的瞬态仍在原始波形。
- 对照 0～0.2、0.2～1、1～3、3～4、4～6 s。每窗须覆盖完整时段且满足实际位置、反馈和时钟要求。已有数据提供短时同步及低速接近证据。用下面的候选矩阵补齐统一接近轨迹下的 ≥3 s 同步保持。
- 窗内计算有符号均值、RMSE、MAE、绝对误差 P95。方向对照按位置、会话、重复轮配对。报告正减负、双向中心偏差和试次间标准差。同一试次的多个窗口或 C 段落共享来源，跨集合划分按原始完整试次处理。

## 推荐补采矩阵

| 项目 | 参数 |
|---|---|
| 机械工况 | Z 轴位置伺服，空载无接触，R 固定 |
| HOLD 位置 | 12,000、16,000、20,000 pulse，即 12、16、20 mm |
| 正向接近 | 8,000 → 12,000，12,000 → 16,000，16,000 → 20,000 pulse |
| 反向接近 | 16,000 → 12,000，20,000 → 16,000，24,000 → 20,000 pulse |
| 转场 | 程序从独立读到的停稳位置转到准备端，速度 2 mm/s，加减速度 20 mm/s² |
| 测量段 | 4 mm，单轴 TRAP，10 mm/s，等加减速 400 mm/s²，平滑参数 0 ms |
| 保持 | 起点先采 250 ms 连续停稳，测量接近后连续停稳 6 s |
| 次数 | 3 个位置 × 2 个方向 × 3 次＝18 个独立到位。第 1 轮 12 mm 正反各 1 次先试采，验收后计入正式重复。每批最多 3 次 |
| 待现场确认的保护默认值 | 0～37,000 pulse，附加停车余量 4,000 pulse，跟随误差上限 500 pulse，最低平滑停减速度 0.99 pulse/ms² |
| 速度保护 | 测量 15 mm/s，转场 5 mm/s |
| 采样与插补 | 读回控制卡核周期为 50 µs 才启动 Watch，每核周期记录编码器、反馈电流和卡内时间。主机状态轮询请求 5 ms，分析使用实测时间。TRAP 由卡内规划器执行，代码未单独设置插补周期 |

## 程序入口和运行

采用原第三阶段 VS 工程，Release x64 选择 `EntryPoint=d_hold`。在工作区根目录打开 PowerShell。

```powershell
$py = 'C:/Users/HYB/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe'
$d = '.\分项实验\03_摩擦与残差补偿'
& $py -B "$d\scripts\d_replay.py" --history
& $py -B "$d\scripts\d_run.py" --plan
$id = 'Z-D-HOLD-01'
& $py -B "$d\scripts\d_run.py" --freeze $id
& $py -B "$d\scripts\d_run.py" --pending $id --pilot
```

上述命令离线执行。核对现场保护默认值后冻结协议。取得本轮现场授权后，运行第一批两次，输入实际现场会话 ID、`READY`，等独立读卡与路线检查通过后输入 `START`。

```powershell
& $py -B "$d\scripts\d_run.py" --batch $id --pilot
& $py -B "$d\scripts\d_run.py" --replay $id
& $py -B "$d\scripts\d_replay.py" --protocol $id
```

试采通过后，去掉 `--pilot` 按批次续采。Ctrl+C 可请求程序平滑停机。中断原始记录保留，按完整独立到位补采。原始数据在 `D/data/raw`，协议及检查记录在 `D/manifest`。历史回放输出 `D/results/historical`，新协议输出 `D/results/<协议ID>`。其中 `hold_windows.csv` 记录逐次窗口和有效性，`hold_coverage.csv` 汇总覆盖，`hold_summary.csv` 汇总误差与重复性，`direction_pairs.csv` 给出新 D 正反向配对结果。

2026-11-27，协议 Z-D-HOLD-01 完成实机采集。18 次全部验收，90 个分层窗口有效。未通过的采集和独立复查均留档，按原判据补采后入账。4～6 s 的正向减反向平均残差在 12、16、20 mm 分别为 58.28、59.00、74.99 raw。 [逐次验收](results/Z-D-HOLD-01/accepted_trials.csv) · [方向配对](results/Z-D-HOLD-01/direction_pairs.csv)
