# 联合建模第三轮

研究对象为 B1、C、D 的空载反馈电流。导读按 A、B、C 顺序。计算和原始信号核验均在离线完成。

1. [实验方案与技术协议](实验方案与技术协议.md) · [机器可读配置](protocol.json)
2. [A 速度反号与状态估计](A_速度反号与状态估计.md)
3. [B MLP 与 LSTM](B_MLP与LSTM对照.md)
4. [C 联合消融、最终建议和最小补充实验](C_联合消融与最终建议.md)

结果位于 [results](results)。`A_dev_*`、`B_*`、`C_dev_*` 是开发结果，`C_internal_*` 是既有内部留出的诊断性结果。模型参数、训练身份哈希和数值预测存于 `results/fit`，其权重是研究候选。原来源的哈希清单、基准逐点验证和选型封条分别为 `input_hashes.json`、`baseline_verification.json`、`selection_seal.json`。

## 复现

复制本目录的脚本、`protocol.json` 和技术协议到仓库同级的新目录，保留原 V1、V2 目录结构与原始数据路径。运行者需提供 Python 3.12、NumPy、pandas 和 CPU PyTorch 2.7.1。PowerShell 入口默认使用原工作区的 Python，也可传 `-Python`。若已在仓库 `tmp/zr3_python` 放置 PyTorch，入口会自动使用该依赖目录。模型仅需 CPU。

```powershell
.\run.ps1 baseline
.\run.ps1 A
.\run.ps1 B
.\run.ps1 C
.\run.ps1 internal
.\run.ps1 report
```

`report` 可从已完成的结果重生成图表。评价保存有均方根、绝对均值、偏差、等试次加权 P95、最大绝对误差及覆盖。预测误差符号定义为测量减预测。运行时与库版本记录在 `results/runtime.json`，库的浮点实现可能产生微小差异。聚合报告生成后，可运行 `python -B verify_run.py` 核对结果掩码、受保护输入及 SVG。源文件及封条按 SHA-256 核对。内部留出只供诊断，具体选型以开发封条为准。

## 文献对应

Zhou 等的无外载时序残差路径用于设计实验 B。第二阶段的接触力监督属于论文自身任务，第三轮的所有监督输出仍是空载电流残差。[Zhou 等，2026](https://doi.org/10.1016/j.conengprac.2026.107140)

Liu 等关于数值微分、模型与测量噪声的分析用于设计实验 A。这里的卡尔曼滤波状态仅为编码器位置、速度和加速度。[Liu 等，2021](https://doi.org/10.1016/j.rcim.2021.102168)

