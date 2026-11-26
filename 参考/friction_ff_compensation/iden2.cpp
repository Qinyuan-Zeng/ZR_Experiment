











































#include <conio.h>
#include <windows.h>
#include <stdio.h>
#include <math.h>     
#include <iostream>
#include "gxn.h"      // 固高头文件

// ================= 宏定义 =================
#define AXIS 1
#define CORE 2        // 使用的是核2

// 采样/控制周期 (ms)
#define LOOP_DELAY_MS 1   

// 错误处理函数
void commandhandler(const char* command, short error)
{
   
       printf("[ERROR] %s = %d\n", command, error);
    
}

// ================= 主函数 =================
int main(int argc, char* argv[])
{
    short sRtn;
    long pSts;
    double prfPos, encPos, prfVel = 0.0, encVel = 0.0;

    printf("系统初始化中...\n");

    // 1. 打开运动控制器
    sRtn = GTN_OpenCard();
    commandhandler("GTN_OpenCard", sRtn);

    // 2. 初始化网络 (加载XML配置)
    sRtn = GTN_NetInit(NET_INIT_MODE_XML_STRICT, const_cast<char*>("C:\\Users\\MCLab\\Desktop\\MotionStudio_x64_V2.3.14.0_develop\\Googol.xml"), 60, &pSts);
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

    Sleep(500); // 等待使能稳定

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
    // 加速度和减速度设置 (若想看理想的纯纯阶跃，可适当调大或保持300)
    jogPrm.acc = 300.0; 
    jogPrm.dec = 300.0;
    jogPrm.smooth = 0.0;  // 关闭平滑减少滞后
    sRtn = GTN_SetJogPrm(CORE, AXIS, &jogPrm);
    commandhandler("GTN_SetJogPrm", sRtn);

    // 初始速度设为0并启动
    sRtn = GTN_SetVel(CORE, AXIS, 0);
    commandhandler("GTN_SetVel", sRtn);
    sRtn = GTN_Update(CORE, 1 << (AXIS - 1));
    commandhandler("GTN_Update", sRtn);

    // ================= 打开文件，准备辨识数据输出 =================
    FILE* fp = nullptr;
#if defined(_MSC_VER)
    fopen_s(&fp, "staircase_identification3.csv", "w");
#else
    fp = fopen("staircase_identification.csv", "w");
#endif

    if (fp == nullptr) {
        printf("无法创建 csv 文件！请检查权限。\n");
        GTN_AxisOff(CORE, AXIS);
        GTN_Close();
        return -1;
    }
    // 写入表头
    fprintf(fp, "time_s,cmd_vel_ms,prf_vel_ms,enc_vel_ms,prf_pos,enc_pos\n");

    printf("\n准备完毕，开始【阶梯速度系统辨识】实验。\n");
    printf("按空格键 (SPACE) 可随时中断并退出，不按会自动跑完。\n\n");

    // ================= 实时控制与记录主循环 =================
    double t_total = 0;
    long start_clock = GetTickCount(); 

    double lastTargetVel = -9999.0;
    double targetVel = 0.0;
    const double MAX_TIME_S = 17.0; // 设定实验跑完所需的总时长

    while (t_total < MAX_TIME_S) {
        t_total = (GetTickCount() - start_clock) / 1000.0;

        // --- A. 生成多组匀速阶梯指令 ---
        // 你可以根据实际所需的电机速度幅值（pulse/ms）调整以下数字
        if (t_total < 1.0) {
            targetVel = 0.0;       // 0-1s：初态打底
        } else if (t_total < 3.0) {
            targetVel = 5.0;       // 1-3s：第一跳（低速正转）
        } else if (t_total < 5.0) {
            targetVel = 15.0;      // 3-5s：第二跳（中速正转）
        } else if (t_total < 7.0) {
            targetVel = 25.0;      // 5-7s：第三跳（高速正转） 
        } else if (t_total < 9.0) {
            targetVel = 0.0;       // 7-9s：回零打底等待发车
        } else if (t_total < 11.0) {
            targetVel = -5.0;      // 9-11s：第一跳（低速反转）
        } else if (t_total < 13.0) {
            targetVel = -15.0;     // 11-13s：第二跳（中速反转）
        } else if (t_total < 15.0) {
            targetVel = -25.0;     // 13-15s：第三跳（高速反转）
        } else {
            targetVel = 0.0;       // 15s后：停止
        }

        // --- B. 边缘触发更新速度 (只在阶段切换的那一瞬间下发) ---
        if (targetVel != lastTargetVel) {
            GTN_SetVel(CORE, AXIS, targetVel);
            GTN_Update(CORE, 1 << (AXIS - 1));
            lastTargetVel = targetVel;
        }

        // --- C. 读取当前系统状态 ---
        GTN_GetPrfPos(CORE, AXIS, &prfPos);
        GTN_GetEncPos(CORE, AXIS, &encPos);
        GTN_GetPrfVel(CORE, AXIS, &prfVel);
        
        // 尝试获取反馈速度，如果函数不支持兼容则给0
        if (GTN_GetAxisEncVel(CORE, AXIS, &encVel) != 0) {
             encVel = 0.0; 
        }

        // --- D. 写入CSV表格 ---
        fprintf(fp, "%.4f,%.4f,%.4f,%.4f,%.0f,%.0f\n", 
                t_total, targetVel, prfVel, encVel, prfPos, encPos);

        // --- E. 终端稀疏打印 ---
        if ((int)(t_total * 1000) % 300 == 0) {
            printf("\rTime: %5.2fs | StepCmd: %6.1f | ActV: %6.1f ", 
                   t_total, targetVel, encVel);
        }

        // --- F. 检测终止键 ---
        if (_kbhit()) {
            if (_getch() == ' ') break;
        }

        Sleep(LOOP_DELAY_MS);
    }

    // ================= 停机与清理 =================
    printf("\n\n测试完成，正在回零并保存数据...\n");

    if (fp) fclose(fp);

    GTN_SetVel(CORE, AXIS, 0.0);
    GTN_Update(CORE, 1 << (AXIS - 1));
    GTN_Stop(CORE, 1 << (AXIS - 1), 0);
    
    Sleep(500);

    GTN_AxisOff(CORE, AXIS);
    GTN_Close();

    printf("运行结束，系统辨识数据完美保存至 staircase_identification.csv!\n");
    _getch();
    return 0;
}