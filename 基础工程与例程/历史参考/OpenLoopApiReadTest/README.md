# GVN 开环力控实验

- [研究进展与轨迹规划](研究进展.md) 汇总已实测结果、数据位置和下一步。
- [SDK 与接口交接](../GVN新增API使用与验证交接.md) 记录补充声明、模式编号、状态检查和异常回切。

## 使用哪个程序

| 用途 | 文件 |
|---|---|
| 现场状态复核 | [OpenLoopStateRead.cpp](OpenLoopStateRead.cpp)，只开卡并读取使能位、模式、目标和切换任务，不重新初始化网络 |
| 接口读取与禁用状态参数读写 | [main.cpp](main.cpp)，是 OpenLoopApiReadTest.sln 当前编译入口 |
| 零输出切换与回切 | [ZeroOutputModeSwitchTest.sln](ZeroOutputModeSwitchTest.sln)，启动后通过初始检查即自动使能 |
| DAC 到反馈 raw 的多档试验 | [DacCurrentMappingTest.cpp](DacCurrentMappingTest.cpp)，0～100 正向 DAC 短脉冲 |
| 单次三角波基线 | [TriangleDacUpdateTest.cpp](TriangleDacUpdateTest.cpp)，已有完整实测日志 |
| 三种电流指令曲线 | [CurrentProfilesTest.cpp](CurrentProfilesTest.cpp)，2 ms 发送与采样，三角波、正偏置正弦、五次 S 型 |
| 作图 | [PlotCurrentProfiles.py](PlotCurrentProfiles.py)，读取三波形 CSV，输出 DAC 阶梯与有符号电流反馈图 |

使用 VS x64 原工程时，只编译所选的一个含 main 的测试源文件。项目构建会复制配套 DLL 和 XML。正式上机前现场核查轴未使能、模式2、目标0、没有待完成的切换，并确认滑台有余量及硬件急停可用。程序中的电流、速度和位移阈值是软件保护。

## 文件位置

- `data/01_接口与状态/` 保存接口、零输出切换、只读状态及 SDK 错误日志。
- `data/02_DAC映射与位移/` 保存单次短脉冲、多档标定及起动探索的原始日志和 CSV。
- `data/03_波形验证/` 保存三角波、1 ms 加密采样尝试和三波形实验数据。
- `figures/` 保存代表性波形图。新运行结果仍写入 VS 的输出目录，可核对后归档至对应 data 子目录。
- `history/` 留存早期试验源码和原交接记录。历史 CPP 如需复查可复制回项目根目录，作为唯一入口编译。
- `sdk/`、XML、扩展头、VS 工程及 `tests/` 保留复现所需的 SDK、接线配置和离线验证材料。

画图时，用带 Pillow 的 Python 执行 `PlotCurrentProfiles.py <实验CSV> [输出PNG]`。图中仅对成功下发的指令与有效反馈画实测曲线，缺测波形标为计划。
