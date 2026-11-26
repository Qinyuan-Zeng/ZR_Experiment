# 基础工程与例程

历史参考/OpenLoopApiReadTest 是配套较完整的旧台参考工程，附SDK、XML和原始验证日志。它是来源副本，执行前需做新台适配。原README中的运行说明只适用于旧台。
| 程序 | 用途与证据 |
|---|---|
| OpenLoopStateRead.cpp | 读取轴状态与返回码；源码轴号需修改 |
| gvn_force_api_ext.h | 新增API补声明，绑定SDK版本 |
| ZeroOutputModeSwitchTest.cpp | 旧台零输出切换实测；竖直轴策略待确认 |
| DacCurrentMappingTest.cpp | 旧台正向短脉冲映射，标定方法参考 |
| TriangleDacUpdateTest.cpp | 旧台2ms三角波实测 |
| CurrentProfilesTest.cpp | 三角/正弦部分实测；S型及最新整轮退出待验证 |
| 分项采集/run_e2_hold_position.cpp | 多位置保持采集结构 |
| 分项采集/run_e3_velocity_motion.cpp | 速度扫描结构 |
| 分项采集/run_e4_acceleration_inertia.cpp | 加减速记录与分析入口参考 |
| 分项采集/run_e5_stop_history.cpp | 出现历史差异后可参考 |
| 分项采集/run_e6a_blind_validation.cpp | 独立验证采集结构 |
| ModelingExperiment3/acquisition/run_m3_controlled_dataset.cpp | 工况分组与数据记录；不继承旧预热流程 |
| CurrentResidualForceCalibration/acquisition/run_spring_mapping.cpp | 独立测力同步与残差映射采集 |
| force_control_platform_test/1_adc_pressure_probe.cpp | 测力通道采集参考 |

分项采集及其余旧工程仅保留选取的源码/工程说明材料，含旧路径和依赖，尚未整理为独立可编译包。完整源工程留在E:/A_ZQY/Experiment同名目录。OpenLoop工程的主入口、SDK与配置齐备，尚未在新工作区编译或接新设备验证。

新增正式项目放分项实验下，一个类别一个项目，多个入口择一编译。旧CPP不自动纳入新实验项目。
