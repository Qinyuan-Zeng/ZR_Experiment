#include <conio.h>
#include <windows.h>
#include <stdio.h>
#include <math.h>
#include <vector>
#include "gxn.h"

// ================= 配置区域 =================
#define CORE 2          // 核心号 (参考 pt_test.cpp)
#define AXIS 1          // 轴号
#define TABLE_ID 1      // PVT表格ID
#define PI 3.141592653589793

// --- 正弦轨迹参数 ---
// 为了保证 PVT 表格能装下，我们生成有限长度的轨迹(例如 10秒)
const double MOTION_FREQ = 0.5;      // 频率 0.5Hz
const double MOTION_AMP = 100000.0;  // 振幅
const double TOTAL_TIME = 20000.0;  // 总时间 10000ms
const int    POINT_COUNT = 100;      // 也就是每 100ms 一个关键点

// --- 摩擦模型参数 ---
struct FrictionParams {
    double Tc, Ts, Tv, Vs, DeadZone;
};
FrictionParams g_fric = { 0.0, 400.0, 0, 9000000.0, 30 };

// --- 您的专用 API 声明 ---
extern "C" {
    short __stdcall GTN_SetFrictionCompensateValue(short core, short control, double velCompensate, double forceCompensate);
}

// --- 辅助函数 ---
void CommandHandler(const char* command, short error) {
    if (error != 0) printf("[ERROR] %s = %d\n", command, error);
}

int Sign(double val) {
    if (val > 1e-9) return 1;
    if (val < -1e-9) return -1;
    return 0;
}

double CalculateFrictionForce(double vel, const FrictionParams& p) {
    double absVel = fabs(vel);
    if (absVel <= p.DeadZone) return 0.0;
    double term_viscous = p.Tv * vel;
    double stribeck_factor = exp(-pow(absVel / p.Vs, 2));
    double term_dry = Sign(vel) * (p.Tc + (p.Ts - p.Tc) * stribeck_factor);
    double total = term_dry + term_viscous;
    if (total > 30000.0) total = 30000.0;
    if (total < -30000.0) total = -30000.0;
    return total;
}

// ================= 主函数 =================
int main(int argc, char* argv[])
{
    short sRtn;
    long pSts;

    // 1. 初始化 (严格照搬 pt_test.cpp 的流程)
    sRtn = GTN_OpenCard();
    CommandHandler("GTN_OpenCard", sRtn);

    // 必须加载 XML 才能连接驱动器
    sRtn = GTN_NetInit(NET_INIT_MODE_XML_STRICT, const_cast<char*>("C:\\Users\\MCLab\\Desktop\\MotionStudio_x64_V2.3.14.0_develop\\Googol.xml"), 60, &pSts);
    CommandHandler("GTN_NetInit", sRtn);
    if (sRtn != 0) {
        printf("网络初始化失败，请检查 XML 路径！\n");
        _getch();
        return -1;
    }

    sRtn = GTN_ClrSts(CORE, AXIS, 1);
    sRtn = GTN_AxisOn(CORE, AXIS);
    sRtn = GTN_ZeroPos(CORE, AXIS);
    sRtn = GTN_SetPrfPos(CORE, AXIS, 0);

    // 2. 生成 PVT 数据表 (正弦波)
    // 我们需要计算 Time, Pos, Vel
    std::vector<double> timeArr(POINT_COUNT + 1);
    std::vector<double> posArr(POINT_COUNT + 1);
    std::vector<double> velArr(POINT_COUNT + 1);

    printf("生成 PVT 正弦数据...\n");
    double dt = TOTAL_TIME / POINT_COUNT; // 时间间隔
    double omega = 2.0 * PI * MOTION_FREQ;

    for (int i = 0; i <= POINT_COUNT; i++) {
        double t_ms = i * dt;
        double t_sec = t_ms / 1000.0;

        // 位置: P = A * sin(w*t)
        posArr[i] = MOTION_AMP * sin(omega * t_sec);

        // 速度: V = A * w * cos(w*t) (单位: pulse/s)
        // 注意：PVT 表格通常需要 velocity 单位与 SetVel 一致，通常是 pulse/ms
        // 所以这里要除以 1000
        double v_pulse_per_sec = MOTION_AMP * omega * cos(omega * t_sec);
        velArr[i] = v_pulse_per_sec / 1000.0;

        timeArr[i] = t_ms;
    }
    sRtn = GTN_PrfPvt(CORE, AXIS);
    CommandHandler(const_cast<char*>("GTN_PrfPvt"), sRtn);
    // 3. 下发 PVT 数据表
    // 参数: core, tableId, count, time[], pos[], vel[]
    sRtn = GTN_PvtTable(CORE, TABLE_ID, POINT_COUNT + 1, &timeArr[0], &posArr[0], &velArr[0]);
    CommandHandler("GTN_PvtTable", sRtn);

    // 4. 选择数据表
    sRtn = GTN_PvtTableSelect(CORE, AXIS, TABLE_ID);
    CommandHandler("GTN_PvtTableSelect", sRtn);

    // 5. 启动运动
    printf("启动 PVT 正弦运动 (持续 %.1f 秒)...\n", TOTAL_TIME / 1000.0);
    sRtn = GTN_PvtStart(CORE, 1 << (AXIS - 1));
    CommandHandler("GTN_PvtStart", sRtn);

    // 6. 实时循环：摩擦补偿
    // 我们利用当前时间推算理论速度，进行前馈补偿
    long sts;
    double current_time = 0;
    long start_tick = GetTickCount();

    do {
        // 获取轴状态
        GTN_GetSts(CORE, AXIS, &sts);

        // 计算当前运行时间 (也可以用 GTN_PvtStatus 读取卡内时间，这里用 Windows 时间简化)
        current_time = (GetTickCount() - start_tick); // ms
        double t_sec = current_time / 1000.0;

        if (current_time < TOTAL_TIME) {
            // A. 计算理论速度 (用于摩擦模型，这样最干净，没噪声)
            // V(t) = A * w * cos(w*t)
            double v_theory_p_ms = (MOTION_AMP * omega * cos(omega * t_sec)) / 1000.0;

            // B. 计算摩擦力
            double fric_force = CalculateFrictionForce(v_theory_p_ms * 1000.0, g_fric); // 传参单位 pulse/s

            // C. 下发补偿
            GTN_SetFrictionCompensateValue(CORE, AXIS, 0.0, fric_force/3.2);

            // 打印调试
            if ((int)current_time % 200 == 0) {
                double prfPos;
                GTN_GetPrfPos(CORE, AXIS, &prfPos);
                printf("T:%.0f ms  Pos:%.0f  Fric:%.0f\r", current_time, prfPos, fric_force);
            }
        }
        else {
            // 时间到，清零补偿
            GTN_SetFrictionCompensateValue(CORE, AXIS, 0.0, 0.0);
        }

        Sleep(2); // 短延时

    } while (sts & 0x400); // 等待规划停止 (0x400通常是运动中标志，参考pt_test.cpp)

    // 7. 结束
    printf("\n运动完成。\n");
    GTN_SetFrictionCompensateValue(CORE, AXIS, 0.0, 0.0);
    GTN_AxisOff(CORE, AXIS);
    GTN_Close(); // 无参数

    _getch();
    return 0;
}