#include <conio.h>
#include <windows.h>
#include <stdio.h>
#include <math.h>
#include <iostream>
#include "gxn.h"      // 固高头文件

// ================= 宏定义 =================
#define AXIS 1
#define CORE 2                 // 使用的是核2
#define LOOP_DELAY_MS 1        // 控制周期 1 ms

// ================= 可调安全参数（务必按设备修改） =================
const double STEP_AMP = 10.0;          // 目标速度幅值：pulse/ms
const double MAX_TIME_S = 6.0;         // 实验总时长
const double RAMP_TIME_S = 1.0;        // 幅值缓升时间：1秒内从0爬到目标速度

const double SOFT_POS_LIMIT = 50000.0; // 软限位：编码器位置上限（pulse）【务必按平台修改】
const double SOFT_VEL_LIMIT = 20.0;    // 实际速度上限（pulse/ms）【务必按平台修改】
const double FOLLOW_ERR_LIMIT = 3000.0;// 跟随误差上限 |prfPos-encPos|（pulse）【务必按平台修改】

// ================= 错误处理函数 =================
void commandhandler(const char* command, short error)
{
    
        printf("[ERROR] %s = %d\n", command, error);
    
}

// ================= 缓升辅助函数 =================
// 让 current 以每次 maxStep 的最大步长逐步逼近 target
double moveToward(double current, double target, double maxStep)
{
    if (target > current + maxStep) return current + maxStep;
    if (target < current - maxStep) return current - maxStep;
    return target;
}

// ================= 安全停机函数 =================
void SafeStop(short core, short axis)
{
    short sRtn;
    sRtn = GTN_SetVel(core, axis, 0.0);
    commandhandler("GTN_SetVel(0)", sRtn);

    sRtn = GTN_Update(core, 1 << (axis - 1));
    commandhandler("GTN_Update(stop)", sRtn);

    sRtn = GTN_Stop(core, 1 << (axis - 1), 0);
    commandhandler("GTN_Stop", sRtn);
}

// ================= 主函数 =================
int main(int argc, char* argv[])
{
    short sRtn;
    long pSts = 0;
    long axisSts = 0;

    double prfPos = 0.0, encPos = 0.0;
    double prfVel = 0.0, encVel = 0.0;
    double followErr = 0.0;

    printf("系统初始化中...\n");

    // 1. 打开运动控制器
    sRtn = GTN_OpenCard();
    commandhandler("GTN_OpenCard", sRtn);
    if (sRtn != 0) {
        printf("GTN_OpenCard 失败，程序退出。\n");
        _getch();
        return -1;
    }

    // 2. 初始化网络 (加载XML配置)
    sRtn = GTN_NetInit(
        NET_INIT_MODE_XML_STRICT,
        const_cast<char*>("C:\\Users\\MCLab\\Desktop\\MotionStudio_x64_V2.3.14.0_develop\\Googol.xml"),
        60,
        &pSts
    );
    commandhandler("GTN_NetInit", sRtn);

    if (sRtn != 0) {
        printf("网络初始化失败，程序退出。\n");
        _getch();
        return -1;
    }

    // 3. 清除状态 & 伺服使能
    sRtn = GTN_ClrSts(CORE, AXIS, 1);
    commandhandler("GTN_ClrSts", sRtn);

    sRtn = GTN_AxisOn(CORE, AXIS);
    commandhandler("GTN_AxisOn", sRtn);

    Sleep(500); // 等待伺服稳定

    // 4. 位置清零
    sRtn = GTN_ZeroPos(CORE, AXIS);
    commandhandler("GTN_ZeroPos", sRtn);

    sRtn = GTN_SetPrfPos(CORE, AXIS, 0);
    commandhandler("GTN_SetPrfPos", sRtn);

    // ================= 初始化 Jog 模式 =================
    sRtn = GTN_PrfJog(CORE, AXIS);
    commandhandler("GTN_PrfJog", sRtn);

    TJogPrm jogPrm;
    sRtn = GTN_GetJogPrm(CORE, AXIS, &jogPrm);
    commandhandler("GTN_GetJogPrm", sRtn);

    // 原始Jog参数保留，但上层仍加入“软件缓升”
    jogPrm.acc = 300.0;
    jogPrm.dec = 300.0;
    jogPrm.smooth = 0.0;
    sRtn = GTN_SetJogPrm(CORE, AXIS, &jogPrm);
    commandhandler("GTN_SetJogPrm", sRtn);

    // 初始速度设为0并启动
    sRtn = GTN_SetVel(CORE, AXIS, 0.0);
    commandhandler("GTN_SetVel(init)", sRtn);

    sRtn = GTN_Update(CORE, 1 << (AXIS - 1));
    commandhandler("GTN_Update(init)", sRtn);

    // ================= 打开文件，准备记录数据 =================
    FILE* fp = nullptr;
#if defined(_MSC_VER)
    fopen_s(&fp, "step_identification_safe.csv", "w");
#else
    fp = fopen("step_identification_safe.csv", "w");
#endif

    if (fp == nullptr) {
        printf("无法创建 step_identification_safe.csv 文件！\n");
        GTN_AxisOff(CORE, AXIS);
        GTN_Close();
        return -1;
    }

    // 写入表头
    fprintf(fp, "time_s,desired_vel_ms,cmd_vel_ms,prf_vel_ms,enc_vel_ms,prf_pos,enc_pos,follow_err,axis_sts\n");

    printf("\n准备完毕，开始带保护的阶跃速度辨识实验。\n");
    printf("按 q/Q：急停并退出\n");
    printf("按 SPACE：正常停止并退出\n\n");

    // ================= 实时控制与记录循环 =================
    double t_total = 0.0;
    DWORD start_clock = GetTickCount();

    // 软件缓升相关变量
    const double dt_s = LOOP_DELAY_MS / 1000.0;
    const double cmdSlewRate = STEP_AMP / RAMP_TIME_S;   // 单位：(pulse/ms)/s
    const double maxDeltaPerLoop = cmdSlewRate * dt_s;   // 每个循环允许改变量

    double desiredVel = 0.0;     // 理想段目标
    double cmdVel = 0.0;         // 实际下发给控制卡的速度命令（带缓升）
    double lastSentVel = 1e100;  // 防止第一次不下发

    bool userNormalStop = false;
    bool userEmergencyStop = false;
    bool protectionStop = false;

    char stopReason[256] = { 0 };

    while (t_total < MAX_TIME_S) {
        t_total = (GetTickCount() - start_clock) / 1000.0;

        // --- A. 段目标速度 ---
        if (t_total < 1.0) {
            desiredVel = 0.0;           // 0~1秒：静止
        }
        else if (t_total < 3.0) {
            desiredVel = STEP_AMP;      // 1~3秒：正向目标
        }
        else if (t_total < 5.0) {
            desiredVel = -STEP_AMP;     // 3~5秒：反向目标
        }
        else {
            desiredVel = 0.0;           // 5~6秒：回零
        }

        // --- B. 软件缓升：cmdVel逐步逼近desiredVel ---
        cmdVel = moveToward(cmdVel, desiredVel, maxDeltaPerLoop);

        // 只有命令变化时才下发
        if (fabs(cmdVel - lastSentVel) > 1e-9) {
            sRtn = GTN_SetVel(CORE, AXIS, cmdVel);
            commandhandler("GTN_SetVel(loop)", sRtn);

            sRtn = GTN_Update(CORE, 1 << (AXIS - 1));
            commandhandler("GTN_Update(loop)", sRtn);

            lastSentVel = cmdVel;
        }

        // --- C. 读取状态 ---
        sRtn = GTN_GetPrfPos(CORE, AXIS, &prfPos);
        if (sRtn != 0) {
            snprintf(stopReason, sizeof(stopReason), "GTN_GetPrfPos 失败，错误码=%d", sRtn);
            protectionStop = true;
            break;
        }

        sRtn = GTN_GetEncPos(CORE, AXIS, &encPos);
        if (sRtn != 0) {
            snprintf(stopReason, sizeof(stopReason), "GTN_GetEncPos 失败，错误码=%d", sRtn);
            protectionStop = true;
            break;
        }

        sRtn = GTN_GetPrfVel(CORE, AXIS, &prfVel);
        if (sRtn != 0) {
            snprintf(stopReason, sizeof(stopReason), "GTN_GetPrfVel 失败，错误码=%d", sRtn);
            protectionStop = true;
            break;
        }

        sRtn = GTN_GetAxisEncVel(CORE, AXIS, &encVel);
        if (sRtn != 0) {
            snprintf(stopReason, sizeof(stopReason), "GTN_GetAxisEncVel 失败，错误码=%d", sRtn);
            protectionStop = true;
            break;
        }

        sRtn = GTN_GetSts(CORE, AXIS, &axisSts);
        if (sRtn != 0) {
            snprintf(stopReason, sizeof(stopReason), "GTN_GetSts 失败，错误码=%d", sRtn);
            protectionStop = true;
            break;
        }

        followErr = prfPos - encPos;

        // --- D. 异常保护 ---
        if (fabs(encPos) > SOFT_POS_LIMIT) {
            snprintf(stopReason, sizeof(stopReason),
                "触发位置保护：|encPos|=%.1f > %.1f", fabs(encPos), SOFT_POS_LIMIT);
            protectionStop = true;
            break;
        }

        if (fabs(encVel) > SOFT_VEL_LIMIT) {
            snprintf(stopReason, sizeof(stopReason),
                "触发速度保护：|encVel|=%.3f > %.3f", fabs(encVel), SOFT_VEL_LIMIT);
            protectionStop = true;
            break;
        }

        if (fabs(followErr) > FOLLOW_ERR_LIMIT) {
            snprintf(stopReason, sizeof(stopReason),
                "触发跟随误差保护：|prfPos-encPos|=%.1f > %.1f", fabs(followErr), FOLLOW_ERR_LIMIT);
            protectionStop = true;
            break;
        }

        // 如果你后续确认了状态字中的“报警位/限位位”，可以在这里继续补充：
        // 例如：
        // if (axisSts & 某个报警掩码) { ... protectionStop = true; break; }

        // --- E. 写CSV ---
        fprintf(fp, "%.4f,%.4f,%.4f,%.4f,%.4f,%.0f,%.0f,%.0f,%ld\n",
            t_total, desiredVel, cmdVel, prfVel, encVel, prfPos, encPos, followErr, axisSts);

        // --- F. 终端稀疏打印 ---
        if ((int)(t_total * 1000) % 200 == 0) {
            printf("\rT:%5.2fs | DesV:%6.2f | CmdV:%6.2f | EncV:%6.2f | Err:%7.1f",
                t_total, desiredVel, cmdVel, encVel, followErr);
        }

        // --- G. 键盘检测 ---
        if (_kbhit()) {
            int ch = _getch();

            if (ch == 'q' || ch == 'Q') {
                userEmergencyStop = true;
                snprintf(stopReason, sizeof(stopReason), "用户按下 q 急停");
                break;
            }
            else if (ch == ' ') {
                userNormalStop = true;
                snprintf(stopReason, sizeof(stopReason), "用户按下 SPACE 正常停止");
                break;
            }
        }

        Sleep(LOOP_DELAY_MS);
    }

    // ================= 停机与清理 =================
    printf("\n\n测试结束，执行停机序列...\n");
    printf("停止原因：%s\n", stopReason[0] ? stopReason : "达到设定实验时长");

    // 急停 / 保护停 / 正常停，统一都先下发0速度并Stop
    SafeStop(CORE, AXIS);
    Sleep(500);

    if (fp) {
        fclose(fp);
        fp = nullptr;
    }

    GTN_AxisOff(CORE, AXIS);
    GTN_Close();

    if (userEmergencyStop) {
        printf("程序已按急停方式退出，数据保存为 step_identification_safe.csv。\n");
    }
    else if (protectionStop) {
        printf("程序因异常保护退出，数据保存为 step_identification_safe.csv。\n");
    }
    else if (userNormalStop) {
        printf("程序已正常停止，数据保存为 step_identification_safe.csv。\n");
    }
    else {
        printf("程序安全完成，数据保存为 step_identification_safe.csv。\n");
    }

    _getch();
    return 0;
}