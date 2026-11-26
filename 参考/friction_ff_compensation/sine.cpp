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
const double MOTION_FREQ = 0.5;      // 频率 0.5Hz
const double MOTION_AMP = 100000.0;  // 振幅
const double TOTAL_TIME = 20000.0;   // 总时间 20000ms (20秒)
const int    POINT_COUNT = 100;      // 数据点数量 (每200ms一个关键点)

// --- 辅助函数 ---
// 错误处理指令，发生错误时打印日志
void CommandHandler(const char* command, short error) {
    if (error != 0) printf("[ERROR] %s = %d\n", command, error);
}

// ================= 主函数 =================
int main(int argc, char* argv[])
{
    short sRtn;
    long pSts;

    // 1. 初始化控制卡
    sRtn = GTN_OpenCard();
    CommandHandler("GTN_OpenCard", sRtn);

    // 加载 XML 配置文件以初始化网络
    sRtn = GTN_NetInit(NET_INIT_MODE_XML_STRICT, const_cast<char*>("C:\\Users\\MCLab\\Desktop\\MotionStudio_x64_V2.3.14.0_develop\\Googol.xml"), 60, &pSts);
    CommandHandler("GTN_NetInit", sRtn);
    if (sRtn != 0) {
        printf("网络初始化失败，请检查 XML 路径！\n");
        _getch();
        return -1;
    }

    // 清除状态、使能轴、清零位置
    sRtn = GTN_ClrSts(CORE, AXIS, 1);
    sRtn = GTN_AxisOn(CORE, AXIS);
    sRtn = GTN_ZeroPos(CORE, AXIS);
    sRtn = GTN_SetPrfPos(CORE, AXIS, 0);

    // 2. 生成 PVT 数据表 (正弦波)
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

        // 速度: V = A * w * cos(w*t)
        // 固高 PVT 表格要求的速度单位通常是 pulse/ms，所以除以 1000
        double v_pulse_per_sec = MOTION_AMP * omega * cos(omega * t_sec);
        velArr[i] = v_pulse_per_sec / 1000.0;

        timeArr[i] = t_ms;
    }

    // 设置为 PVT 模式
    sRtn = GTN_PrfPvt(CORE, AXIS);
    CommandHandler("GTN_PrfPvt", sRtn);

    // 3. 下发 PVT 数据表
    sRtn = GTN_PvtTable(CORE, TABLE_ID, POINT_COUNT + 1, &timeArr[0], &posArr[0], &velArr[0]);
    CommandHandler("GTN_PvtTable", sRtn);

    // 4. 选择数据表
    sRtn = GTN_PvtTableSelect(CORE, AXIS, TABLE_ID);
    CommandHandler("GTN_PvtTableSelect", sRtn);

    // 5. 启动运动
    printf("启动 PVT 纯正弦运动 (无摩擦补偿), 持续 %.1f 秒...\n", TOTAL_TIME / 1000.0);
    sRtn = GTN_PvtStart(CORE, 1 << (AXIS - 1));
    CommandHandler("GTN_PvtStart", sRtn);

    // 6. 实时循环：仅仅读取和监控状态
    long sts;
    double current_time = 0;
    long start_tick = GetTickCount();

    // 控制台光标隐藏（防止刷新时闪烁）
    printf("\n监控数据中...\n");

    do {
        // 获取轴状态
        GTN_GetSts(CORE, AXIS, &sts);

        current_time = (GetTickCount() - start_tick);

        // 每隔约 100ms 刷新一次界面显示
        if ((int)current_time % 100 < 5) {
            double prfPos = 0, encPos = 0;
            GTN_GetPrfPos(CORE, AXIS, &prfPos); // 规划的理论位置
            GTN_GetEncPos(CORE, AXIS, &encPos); // 编码器实际反馈位置

            // 误差 = 理论位置 - 实际位置
            double error = prfPos - encPos;

            printf("\rT:%5.0f ms | 规划位置:%7.0f | 实际位置:%7.0f | 跟随误差:%6.0f  ",
                current_time, prfPos, encPos, error);
        }

        Sleep(2);

    } while (sts & 0x400); // 0x400 是运动中标志，运动停止后跳出循环

    // 7. 结束清理
    printf("\n\n运动完成。\n");
    GTN_AxisOff(CORE, AXIS);
    GTN_Close();

    printf("按任意键退出...");
    _getch();
    return 0;
}