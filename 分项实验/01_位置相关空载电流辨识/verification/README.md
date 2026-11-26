# 第一阶段开发验证

## 编译

2026-11-24 使用 VS 2022 Professional、v143、Windows SDK 10 和项目引用的配套 x64 SDK 构建。各组合均为 0 警告、0 错误。

| 入口 | Debug x64 | Release x64 |
| --- | --- | --- |
| read_state | [通过](read_state-Debug.log) | [通过](read_state-Release.log) |
| hold_positions | [通过](hold_positions-Debug.log) | [通过](hold_positions-Release.log) |
| hold_timeseries | [通过](hold_timeseries-Debug.log) | [通过](hold_timeseries-Release.log) |
| scan_velocity | [通过](scan_velocity-Debug.log) | [通过](scan_velocity-Release.log) |

## 离线点表与回放

- [点表预览日志](preview.log)记录 7 个协议预览。A0 为 8 次方向保持，B 为 160 次方向保持，C 为 30 趟扫描，D 为 48 次方向保持。越界的 A2 接近点和 D 目标均被拒绝。
- [离线回放日志](offline.log)覆盖原地假换向、缺测、保持未稳定、中断和未开始试次。还将 C 的开发组重新标记为留出组，以验证验收会拒绝重叠，测试后已恢复样例标签。
- [合成原始数据与结果](sample)包含 A0、A1、A2、B、C、D 的样例 CSV、manifest、冻结模型和评价报告。合成曲线按二次趋势生成，示例选择二次模型，有效范围为 2000～30000 脉冲。验收输出 606 个配对值，其中 D 未见与跨时段各 24 个，其余是三个速度的扫描位置网格值。
- [开发组对照图](sample/data/figures/development.html) · [独立评价表](sample/results/acceptance_metrics.csv) · [验收对照图](sample/data/figures/acceptance.html)

从项目目录复跑时为样例选择一个新的输出目录，例如：

```powershell
node scripts/offline_sample.mjs verification/sample-rerun
```

## 简化控制台复验

stage1-2 重建四个入口的 Debug 和 Release 组合，8 次均为 0 警告、0 错误。[简化点表日志](simplified-preview.log)覆盖 7 个常规预览、高级参数覆盖、越界拒绝、正式运动参数留空拒绝和启动前取消。只读新版实机回归见[控制台日志](simplified-read.log)，回车即按默认索引 1 采集，323 条样本完整，平均周期为 15.54 ms。

## A0 反馈边界复验

stage1-5 已编译静态保持入口的 Debug 和 Release。编译期检查覆盖端点 32003、32010、−10 脉冲及容差外的 32011、−11 脉冲，同时检查余量只匹配对应端点。合成回放验证 −3 和 32003 脉冲的静止保持可纳入统计。[Debug 点表](a0-boundary-Debug.log)和[Release 点表](a0-boundary-Release.log)均保留完整 8 次保持和 0～32000 脉冲指令范围。

## A2 判据和 B 点表复验

- stage1-6 已以合成回放检查 A2 速度尖峰、异常位移和无效保持的处理，保留[点表预览](a2-direction-Release.log)及[回放日志](a2-offline.log)。
- stage1-7 的 A2 为 30 次、每次 4 s。[离线回放](a2-4s-offline.log)覆盖第 3、4 秒均值、第 4 秒与末段均值的一致性，以及旧 2 s 会话字段兼容。[A2 点表](a2-4s-Release.log)
- stage1-8 的 B 为 160 次、每次 4 s，取末 1 s。Debug 和 Release 均通过编译和点表预览。[Debug 点表](b-4s-Debug.log) · [Release 点表](b-4s-Release.log)
- stage1-9 的 B 沿用 A2 的方向、停稳判据和 A0 的局部端点余量。Debug 和 Release 均通过编译及 A0/A2/B 点表复验。B 合成回放包含端点反馈偏差和保持中的速度尖峰，得到 75 个双向配对值、10 个端点单向值，无 B 试次被排除。[B 点表](b-criteria-Release.log) · [离线回放](b-criteria-offline.log)

## C 点表、采样与匀速筛选

- stage1-10 的 C 在 Debug 和 Release 均通过 30 趟点表验证。40000 脉冲/s 配 100000 脉冲/s² 的理论匀速距离为 16000 脉冲。配 20000 脉冲/s² 时拒绝启动。[点表验证](c-preview-Release.log)
- 合成回放包含编码器量化和反向速度尖峰。各速度的开发组共 9 个完整往返均通过匀速筛选，速度以编码器位置和时间戳估计。[离线回放](c-offline.log)
- 新台 5 s 只读测试使用高分辨率计时器，请求 5 ms。共 847 条样本，无缺测。实际均值 5.91 ms，P95 6.12 ms，最大 6.49 ms。[控制台记录](c-5ms-read.log) · [会话记录](../manifest/Z-READ_20261124-161158_11.txt)
- stage1-11 在 Debug 和 Release 验证了 C 的新候选。400000 脉冲/s² 配 40000 脉冲/s 时，理论匀速长度为 28000 脉冲，内收估计为 4000～28000 脉冲。200000 脉冲/s² 的备用组合及 20000 脉冲/s² 时拒绝该高速档也通过预览测试。新候选尚待 C 实机运行。[长匀速段点表](c-long-plateau-Release.log)
- stage1-12 的编译期检查覆盖从 32002 脉冲反向离开上端点、从 −2 脉冲正向离开下端点、超出余量的拒绝和回到工作区间后的撤销。两种构建均保留 C 的 30 趟点表。[Debug 点表](c-reversal-Debug.log) · [Release 点表](c-reversal-Release.log)

## 新平台实测

| 试验 | 实测结果 |
| --- | --- |
| 只读 | 控制环索引 1 可读，请求 10 ms 的一次测量实际均值 15.56 ms。[会话记录](../manifest/Z-READ_20261124-105929_02.txt) |
| A0 | 两次边界中止分别记录到 32001 和 32003 脉冲。采用局部端点余量的 stage1-5 已完成 8 次保持。[正式试次](../manifest/Z-A0_20261124-142633_01.trials.csv) |
| A1 | 3 个连续保持点、9 段，共 6639 条样本，无缺测。[正式试次](../manifest/Z-A1_20261124-144034_01.trials.csv) |
| A2 | 早期会话曾因速度反馈与位置轨迹矛盾而中止。修订后已完成 2 s 对照和正式 4 s 会话，后者 30 次均有效，14641 条样本无缺测。[中断归档](../archive/manifest/Z-A2_20261124-144346_01.txt) · [正式试次](../manifest/Z-A2_20261124-150201_03.trials.csv) |
| B | 粗网格 5 组 160 次全部有效，55994 条样本无缺测。内部双向配对值 75 个，端点单向值 10 个。[正式试次](../manifest/Z-B_20261124-153415_01.trials.csv) |
| C | 首轮在第一次反向时误触发越界。修复后 stage1-12 实机复跑 30 趟全部有效，39682 条样本无接口缺测。40000 脉冲/s 的共同匀速范围为 3420～28510 脉冲。[中止会话归档](../archive/manifest/Z-C_20261124-164002_01.txt) · [完整会话](../manifest/Z-C_20261124-165139_02.txt) |
| D | 待实机采集。 |

## B 加密网格验证

stage1-13 的 B 主网格两轮各 602 次，第二轮交换起始方向。BL、BH 各 11 个单向点，BE 默认 12 个双向点。离线回放覆盖 301 个主网格节点建模、边界协议分离、重合点按方向比较、旧 2000 脉冲网格兼容和 D 训练点拒绝。[Release 点表](b-dense-Release.log) · [加密回放](b-dense-offline.log) · [旧网格回放](b-legacy-offline.log)

## D v1 验收准备

stage1-14 的静态保持入口在 Debug 和 Release 均通过编译。点表确认 D1 为 102 次、D2 为 48 次，均保持 4 s。[D1 点表](D1-Release.log) · [D2 点表](D2-Release.log)

离线回放覆盖 301/38 节点、方案③权重、留出组隔离、训练节点拒绝、域外缺省、缺方向保留及三个图像输出。[离线回放](d-offline.log)

实测 B、C 重建的 301 个 B 均值和方案③ 38 个节点的各来源分量均与已有对照表一致。冻结后先完成 D3 的 6 个留出往返、687 个配对网格值，随后完成 D1、D2 的 150 次到位，共 75 个静态双向配对值。[D3 覆盖](../results/D/Z-D_20261124-200654_01/Z-D_20261124-200654_01.scan_coverage.csv) · [合并验收指标](../results/D/Z-D_20261124-203450_02/Z-D_20261124-203450_02.full_metrics.csv)
