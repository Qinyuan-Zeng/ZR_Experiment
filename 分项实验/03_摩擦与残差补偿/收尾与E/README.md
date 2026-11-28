# 第三阶段收尾与 E 执行说明

适用 [2026-11-28 方案](../第三阶段收尾与E独立验证方案.md)。本次只编译和离线验证。历史数据、候选权重、工程选型阈值、E 判级阈值和实验规模保持原方案。

## 代码入口

| 文件 | 作用 |
|---|---|
| [plan.py](plan.py) | 固定补测与 E 点表、双会话镜像顺序、几何及停车预算 |
| [frozen_replay.py](frozen_replay.py) | 校验 T0/T1 权重和机理哈希，复用 V3 S0/S2、各自历史和特征，按自身归一化及权重推理 |
| [replay.py](replay.py) | 原生 Watch、卡 tick、SDK 状态、TRAP 回读、完整停留与端点检查，生成冻结口径格点 |
| [evaluate.py](evaluate.py) | 补测事件与工程容差、共同／各自支持域、E 的分会话和模板统计、峰值及三级评分 |
| [run_supplement.py](run_supplement.py) · [run_e.py](run_e.py) | 独立控制台入口，由 [controller.py](controller.py) 执行冻结、状态校核、验收、选型与封存 |
| [supplement_scan.cpp](../src/supplement_scan.cpp) · [e_scan.cpp](../src/e_scan.cpp) | 独立 C++ 主入口，复用 [closeout_capture.cpp](../src/closeout_capture.cpp) 中的 Watch、保护监测、TRAP 和连续停稳采集 |
| [selftest.py](selftest.py) | 点表、镜像、合成 Watch、断帧拒收、选型与判级边界自检 |

独立记录按原二维建模口径重置历史，S-A 每腿前保留起始静止预滚动。同一 E 轨迹内的多个段共享一条历史流，途中显式等待保留在 Watch。严格得分沿用 V3 原始格点掩码并检查当前候选支持，T1 新增点另计。无有效样本时报告证据不足。速度 SDK 日志保留专用卡时标、主机读取起止与返回码。Watch 仍以 50 µs 卡时钟保存位置和电流。能否提供等效、及时的 5 ms 在线位置流需实机核验。

## 离线验证

- 现有 VS 项目 `ZRFrictionCurrent.vcxproj` 分别指定 `EntryPoint=supplement_scan` 和 `EntryPoint=e_scan`，两套 x64 Debug、Release 均编译成功。
- T0/T1 各逐点复现历史内部留出的 **117,066** 格点，数值存在模式一致，最大预测差均为 **0 raw**。历史原掩码有 **66,822** 点，T1 与原掩码交集 **66,821** 点。[逐点复现结果](historical_reproduction.json)
- [补测预览](supplement_preview.csv) 有 18 个槽位、28 段，其中 S-A 8 往返、S-B 2 个连续换向试次、H 8 次。 [E 预览](E_preview.csv) 有 24 条、42 段。初始 16,000 pulse、响应时间 0 ms 作为离线预算参考，最小名义匀速长度分别为 1.75 和 2.44 mm，最小测量几何余量分别为 6,227.37 和 4,727.37 pulse。实际响应延迟必须现场确认，正式冻结和批前都会重新核算。
- 合成测试覆盖 50 µs tick 回绕、断帧拒收、双候选评分、镜像顺序、带符号反向检测延迟、工程选型门槛和 E 评分边界。合成的反向事件只验证软件逻辑。[离线自检结果](offline_verification.json)

## 连续换向待验证

已编译独立 PVT 功能验证入口。四条名义轨迹和合成 Watch 验收已通过，正式采集等待本台工程验证和后端接入。[卡侧换向验证说明](卡侧换向验证.md)

S-B 和 E2 要求规划器负责无显式停留的段间衔接。现有 B1/C/D 的单轴 TRAP 路径先等待停稳后由主机发下一段。SDK 头及当前 x64 DLL 有 `GTN_MovePosTwoSegment`、`GTN_MultiMovePosTwoSegment`、`GTN_BufMoveSpEx` 和 `GTN_BufMoveSpSetPrmEx`。已核对声明和 DLL 导出，当前仓库尚缺本台固件下能验证段间反向、两段独立速／加减速、卡侧时间点和安全停车的等价用法。顺序发两次已验证 TRAP 会引入主机停稳和调度等待，轨迹口径随之改变。

最小修改是补一条经过核实的卡侧缓冲执行路径，用厂商定义确认模式和参数单位。在另行授权的验证中先核对两段读回、零速事件、真实反向及停止保护，随后把该后端纳入同一 Watch 和整条轨迹验收。无须修改轨迹点、工况数、权重或评分阈值。现版将 2 个 S-B 和 6 个 E2 槽位标记待验证，运行时拒绝用未验证的后端执行。补测选型要求所有 18 槽位完整，所以 E 正式采集整体保持锁定。

## 运行顺序

从工作区根目录执行以下命令只做离线复现和预览：

```powershell
$py = 'C:/Users/HYB/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe'
$p = '.\分项实验\03_摩擦与残差补偿\收尾与E'
& $py -X utf8 -B "$p/frozen_replay.py"
& $py -X utf8 -B "$p/selftest.py"
& $py -X utf8 -B "$p/run_supplement.py" preview
& $py -X utf8 -B "$p/run_e.py" preview
```

后续现场步骤如下：

1. 用补测入口 `freeze` 登记选型签署者、现场保护和响应时间，封存为 `Z-CLOSEOUT-S01`。现场确认 Z 空载停稳、R 固定、限位、急停、抱闸、保护参数及允许的槽位，随后由补测入口 `batch` 逐槽位执行。先输入 `READY`，独立读卡和实际转场预算通过后再输入 `START`。不完整试次保留现场文件并按冻结质量口径续采。全程以空载位置伺服运行。
2. 18 槽位验收齐全后，用补测入口 `select` 按冻结阈值计算选择。完成严重异常和用途限制人工审查后用 `seal-e` 签署唯一候选，生成 `Z-CLOSEOUT-E01`。连续换向后端还须已通过验证并绑定所用版本。
3. 两次独立就绪按 E 入口 `batch` 采集。每会话应有不同实际会话 ID，并记录独立开机或跨日证据。24 条质量验收齐全，人工确认数据封存后用 E 入口 `score` 只评分一次。先保存评分开始标记，按两会话及汇总给出最差等级、有效域、峰值前后窗口和待审查用途限制。等效 gf 按 0.59558 gf/raw 换算，仅表示电流误差等效值。

本次尚未生成现场协议、签署选型或启动设备。S-A/H 已通过本次离线编译和代码自检，实际 TRAP 回读、停稳和原生采样仍以采集现场验收为准。
