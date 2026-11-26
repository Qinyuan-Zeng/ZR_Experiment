# GVN 新增力控接口交接

当前结论、实测证据和轨迹规划路线统一见 [研究进展](OpenLoopApiReadTest/研究进展.md)。运行入口和归档布局见 [实验目录说明](OpenLoopApiReadTest/README.md)。旧试验的逐次记录见 [历史交接](OpenLoopApiReadTest/history/GVN新增API历史记录.md)。其中的旧路径和设备状态仅供追溯。

## SDK 与补充声明

- 厂家参考包括 [新增指令说明](参考/GVN闭环力控相关指令与例程说明251208.docx)、[vc_gts](参考/vc_gts/) 和 [开环 demo](参考/开环力控demo/)。
- 本套 vc_gts 的 x86、x64 DLL 及导入库均包含说明中的 12 个接口。原头文件仅声明其中 5 个。缺少声明时，先对照厂家文档和导出符号核对 ABI，再在独立扩展头补声明。
- 缺少的声明为 Set/GetForceControlParameter、Set/GetForceCalibrationTable、Set/GetForceLoopPid、GetForceFeedbackValue，以及 TForceControlPrm 和 TForceCalibrationTable。当前程序仅按实际需要补了读取接口和相关结构体。
- Windows 使用厂家 GT_API 调用约定，同架构配套使用头文件、LIB 和 DLL。MSVC 默认对齐下，TForceControlPrm 为 56 字节，feedbackForceK 偏移 8，outputReverse 偏移 48，TSwitchLoopParameter 为 32 字节。实现见 [扩展头](OpenLoopApiReadTest/gvn_force_api_ext.h)。
- 旧 gxn demo 的反馈补偿接口及 controlType 注释与新版有差异。新版说明中 controlType=0 为开环，2 为力双闭环。旧工程换库时需重新核对包含、链接和结构体。
- 返回 1 是 CMD_ERROR_EXECUTE，7 是参数错误，8 是未知或不支持指令。分别判断声明、链接、设备执行、读回及实际输出。NetInit 返回 0 时还需核对是否实际连上总线。

## 模式、单位与退出

- 实验配置为 PCIe channel=5、CORE=2、AXIS=1、INDEX=1，沿用 `GTN_NetInit(100, Googol.xml, 60, &status)`。状态诊断程序只开卡与读取。下层驱动器原有同步周期电流配置由现场核对。
- 当前设备实测模式为 2→3→2。进入任务用切换参数 mode=0，零输出触发，成功提交后读 enable 归零并记录 loopMode。mode 是任务类型，loopMode 是轴状态。新版文档中的模式4及其对应闭环行为仍需独立核对。
- 开环 SetForceValue 与 openLoopDac 的单位为 DAC bit。GetForceValue 读取目标，GetAtlTorque 提供有符号反馈 raw。负目标能否输出反向电流、raw 到安培及接触力换算都需独立证据。400/500 raw 等历史保护阈值是软件检查，额定电流常量不足以确定 raw 到安培比例。
- 正常退出先写零并读回，保持使能提交 mode=1，确认 enable=0 且 loopMode=2 后撤使能，再读回原切换参数。demo 回切 posOffset=-10；本阶段程序不发回退轨迹。异常保护先归零、撤使能。禁用后回切曾超时，必须用读数确认结果。
- 最新三波形源码对归零观察中的短时迟到顺延下一拍。严重零阶段超期经实时状态、零目标和切换任务复查后才走正常回切。该分支有假 API 验证，改动后的整轮实机结果待补。
- 上机前复核实时状态，禁止代理自动执行会使能或驱动电机的程序。现场测试要确认硬件急停可用。QPC 和软件轮询负责记录及保护，通信阻塞可能延长指令保持时间。

历史日志从原 `OpenLoopApiReadTest/bin` 移到 `OpenLoopApiReadTest/data`，按接口状态、DAC 映射与位移、波形验证分组，文件名保留。旧周汇报与厂商参考文件保留原位。
