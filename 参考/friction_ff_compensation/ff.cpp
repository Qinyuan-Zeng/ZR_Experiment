#include <conio.h>
#include <windows.h>
#include <stdio.h>
#include <math.h>     // 引入数学库用于 sin/cos
#include <iostream>
#include "gxn.h"      // 固高头文件

// ================= 宏定义 =================
#define AXIS 1
#define CORE 2        // 使用的是核2
#define PI 3.141592653589793

// 采样/控制周期 (ms)
#define LOOP_DELAY_MS 1   

// --- 正弦轨迹参数 ---
// 目标：位置 P(t) = Amp * sin(2*pi*Freq*t)
const double MOTION_FREQ = 0.5;     // 频率 0.5Hz (2秒一圈)
const double MOTION_AMP = 100000.0; // 振幅 (脉冲)

// --- 摩擦模型参数 ---
struct FrictionParams {
    double Tc;       // 库伦摩擦
    double Ts;       // 静摩擦
    double Tv;       // 粘滞摩擦
    double Vs;       // Stribeck 临界速度
    double DeadZone; // 死区
};

FrictionParams g_fric = {
    300.0,   // Tc
    400.0,   // Ts
    2.0,     // Tv
    10.0,    // Vs
    0.05     // DeadZone
};



// ================= 辅助函数 =================

// 错误处理函数
void commandhandler(const char* command, short error)
{
    if (error != 0) {
        printf("[ERROR] %s = %d\n", command, error);
    }
}

// 符号函数
int Sign(double val) {
    if (val > 1e-9) return 1;
    if (val < -1e-9) return -1;
    return 0;
}

// 摩擦力计算 (计算 Force 部分)
double CalculateFrictionForce(double vel, const FrictionParams& p) {
    double absVel = fabs(vel);

    if (absVel <= p.DeadZone) return 0.0;

    // 1. 粘滞摩擦
    double term_viscous = p.Tv * vel;

    // 2. 库伦 + Stribeck
    double stribeck_factor = exp(-pow(absVel / p.Vs, 2));
    double term_dry = Sign(vel) * (p.Tc + (p.Ts - p.Tc) * stribeck_factor);

    double total = term_dry + term_viscous;

    // 简单的安全限幅
    if (total > 30000.0) total = 30000.0;
    if (total < -30000.0) total = -30000.0;

    return total;
}

// ================= 主函数 =================
int main(int argc, char* argv[])
{
    short sRtn;
    long pSts;
    double prfPos, encPos;

    printf("系统初始化中...\n");

    // 1. 打开运动控制器
    sRtn = GTN_OpenCard();
    commandhandler("GTN_OpenCard", sRtn);

    // 2. 初始化网络 (加载XML配置)
    // 路径保留你代码中的原样
    sRtn = GTN_NetInit(NET_INIT_MODE_XML_STRICT, const_cast<char*>("C:\\Users\\MCLab\\Desktop\\MotionStudio_x64_V2.3.14.0_develop\\Googol.xml"), 60, &pSts);
    commandhandler("GTN_NetInit", sRtn);

    if (sRtn != 0) {
        printf("网络初始化失败，程序退出。\n");
        int ch = _getch();
        printf("您按下了字符: %c\n", ch);
        return -1;
    }

    // 3. 清除状态 & 伺服使能
    sRtn = GTN_ClrSts(CORE, AXIS, 1);
    commandhandler("GTN_ClrSts", sRtn);

    sRtn = GTN_AxisOn(CORE, AXIS);
    commandhandler("GTN_AxisOn", sRtn);

    // 4. 位置清零
    sRtn = GTN_ZeroPos(CORE, AXIS);
    commandhandler("GTN_ZeroPos", sRtn);
    sRtn = GTN_SetPrfPos(CORE, AXIS, 0);
    commandhandler("GTN_SetPrfPos", sRtn);

    // =========================================================
    // 关键修改：为了跟踪正弦波，我们将模式从 PrfTrap 改为 PrfJog
    // =========================================================

    // 5. 设置为 Jog (速度) 模式
    sRtn = GTN_PrfJog(CORE, AXIS);
    commandhandler("GTN_PrfJog", sRtn);

    // 6. 设置 Jog 参数
    TJogPrm jogPrm;
    sRtn = GTN_GetJogPrm(CORE, AXIS, &jogPrm);
    jogPrm.acc = 300.0; // 加速度设大一点，以便快速跟随正弦波变化
    jogPrm.dec = 100.0;
    jogPrm.smooth = 0;  // 平滑设为0，减少滞后
    sRtn = GTN_SetJogPrm(CORE, AXIS, &jogPrm);
    commandhandler("GTN_SetJogPrm", sRtn);

    // 7. 初始速度设为0并启动
    sRtn = GTN_SetVel(CORE, AXIS, 0);
    commandhandler("GTN_SetVel", sRtn);

    sRtn = GTN_Update(CORE, 1 << (AXIS - 1));
    commandhandler("GTN_Update", sRtn);

    printf("运动已启动，开始正弦跟踪 + 摩擦补偿...\n");
    printf("按空格键 (SPACE) 停止并退出。\n\n");

    // ================= 实时控制循环 =================

    double t_total = 0;
    long start_clock = GetTickCount(); // 记录开始时间

    while (true) {
        // --- A. 计算正弦轨迹对应的速度 ---
        // P(t) = A * sin(w*t)a
        // V(t) = A * w * cos(w*t) (位置的导数)
        double omega = 2.0 * PI * MOTION_FREQ;

        // 计算目标速度 (假设单位是 pulse/ms，需要根据cos的pulse/s换算)
        // 注意：cos的结果是 pulse/s，除以1000变为 pulse/ms
        double target_vel_ms = (MOTION_AMP * omega * cos(omega * t_total)) / 1000.0;

        // --- B. 计算摩擦前馈 ---
        // 计算 Force 补偿值
        double friction_force = CalculateFrictionForce(target_vel_ms * 1000.0, g_fric);

        // --- C. 下发指令 ---

        // 1. 下发速度 (驱动运动)
        GTN_SetVel(CORE, AXIS, target_vel_ms);
        GTN_Update(CORE, 1 << (AXIS - 1));

        // 2. 下发摩擦补偿 (使用你的 API)
        // 参数3(vel): 0, 参数4(force): friction_force
        //GTN_SetFrictionCompensateValue(CORE, AXIS, 0.0, friction_force);

        // --- D. 打印状态 (每200ms) ---
        if ((int)(t_total * 1000) % 200 == 0) {
            GTN_GetPrfPos(CORE, AXIS, &prfPos);
            GTN_GetEncPos(CORE, AXIS, &encPos);
            // 打印: 时间, 规划位置, 实际位置, 误差, 补偿力
            printf("T:%.2f P_Prf:%.0f Err:%.1f F_Comp:%.0f\r",
                t_total, prfPos, prfPos - encPos, friction_force);
        }

        // --- E. 退出检测 ---
        if (_kbhit()) {
            int ch = _getch();
            if (ch == ' ') break; // 空格键退出
        }

        Sleep(LOOP_DELAY_MS);

        // 更新时间
        t_total = (GetTickCount() - start_clock) / 1000.0;
    }

    // ================= 停机与清理 =================
    printf("\n停止运动...\n");

    // 1. 清除摩擦补偿
    GTN_SetFrictionCompensateValue(CORE, AXIS, 0.0, 0.0);

    // 2. 停止轴
    GTN_Stop(CORE, 1 << (AXIS - 1), 0);

    // 3. 关闭伺服
    GTN_AxisOff(CORE, AXIS);

    // 4. 关闭控制器
    GTN_Close(); // 修正为无参数

    printf("程序结束。\n");
    _getch();
    return 0;
}