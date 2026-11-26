/*==============================================================
 * ILCBF-GPPDF 第一阶段：离散点迭代学习控制(ILC)数据采集程序（修正版）
 * 编译环境：Visual Studio (C++) / 确保链接 gxn.lib 并包含 gxn.h
 *==============================================================*/
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <Windows.h>
#include <mmsystem.h>
#include "gxn.h"

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "gxn.lib")

#define CORE            2
#define AXIS            1
#define PROFILE_NUM     1       // 规划器号，单轴控制下通常与轴号一致

// ILC 超参数（严格对应 MATLAB 仿真）
#define NUM_POSITIONS   4       
#define NUM_TRIALS      20      
#define ILC_L           0.6     
#define ILC_Q           0.98    

// 物理行程与运动增益映射
#define MAX_STROKE      3669016.0 // 对应归一化位置 1.0 的实际脉冲数
#define EXCITATION_AMP  2000.0   // 局部往复激励幅值（脉冲）
#define ERROR_GAIN      0.005    // 跟踪误差到控制卡前馈力矩的转换系数

TTrapPrm g_trap_prm;

/*==============================================================
 * 辅助函数：让轴精准运动到绝对位置，并等待停止（修正 API）
 *==============================================================*/
void move_to_position(double target_pos) {
    long sts;
    // 1. 切换为 Trap 模式
    GTN_PrfTrap(CORE, PROFILE_NUM);
    
    // 2. 配置默认加减速
    GTN_GetTrapPrm(CORE, PROFILE_NUM, &g_trap_prm);
    g_trap_prm.acc = 0.2; // 脉冲/ms^2
    g_trap_prm.dec = 0.2;
    g_trap_prm.smoothTime = 20;
    GTN_SetTrapPrm(CORE, PROFILE_NUM, &g_trap_prm);
    
    // 3. 【修正】使用标准的 Prf 接口设置目标位置与速度
    GTN_SetPrfPos(CORE, PROFILE_NUM, target_pos); 
    GTN_SetPrfVel(CORE, PROFILE_NUM, 20.0);       // 寻位速度：20 脉冲/ms
    
    // 4. 点火更新
    GTN_Update(CORE, (1L << (PROFILE_NUM - 1)));
    
    // 5. 等待轴规划停止
    do {
        GTN_GetPrfSts(CORE, PROFILE_NUM, &sts, 1, NULL);
        Sleep(10);
    } while (sts & (1L << 11)); // 第11位为1表示在运动
    
    Sleep(200); // 停稳机械时间
}

/*==============================================================
 * 核心函数：执行一次局部激励往复运动，并捕获本轮 Trial 的跟踪误差 e_j
 *==============================================================*/
double execute_trial_and_get_error(double base_pos) {
    long sts;
    double total_error = 0.0;
    int sample_count = 0;
    double current_error = 0.0;

    // 1. 发送正向激励位置
    GTN_SetPrfPos(CORE, PROFILE_NUM, base_pos + EXCITATION_AMP);
    GTN_SetPrfVel(CORE, PROFILE_NUM, 10.0); // 激励速度稍微放缓
    GTN_Update(CORE, (1L << (PROFILE_NUM - 1)));

    // 采样正向运动误差
    do {
        GTN_GetAxisError(CORE, AXIS, &current_error, 1, NULL);
        total_error += fabs(current_error); // 累加绝对跟踪误差
        sample_count++;
        Sleep(2);
        GTN_GetPrfSts(CORE, PROFILE_NUM, &sts, 1, NULL);
    } while (sts & (1L << 11));

    // 2. 发送反向指令回到基准点
    GTN_SetPrfPos(CORE, PROFILE_NUM, base_pos);
    GTN_Update(CORE, (1L << (PROFILE_NUM - 1)));

    // 采样反向运动误差
    do {
        GTN_GetAxisError(CORE, AXIS, &current_error, 1, NULL);
        total_error += fabs(current_error);
        sample_count++;
        Sleep(2);
        GTN_GetPrfSts(CORE, PROFILE_NUM, &sts, 1, NULL);
    } while (sts & (1L << 11));

    return (sample_count > 0) ? (total_error / sample_count) : 0.0;
}

int main() {
    long Status;
    timeBeginPeriod(1);

    double P_train_norm[NUM_POSITIONS] = {0.05, 0.35, 0.65, 0.95}; 
    double P_train_pulse[NUM_POSITIONS];
    double theta_train[NUM_POSITIONS] = {0.0, 0.0, 0.0, 0.0}; 

    for(int i = 0; i < NUM_POSITIONS; i++) {
        P_train_pulse[i] = P_train_norm[i] * MAX_STROKE;
    }

    printf("============================================\n");
    printf("  第一阶段：离散点 ILC 参数在线学习与采集（修正版）\n");
    printf("============================================\n");

    // 初始化控制卡
    if (GTN_OpenCard() != 0) { printf("开卡失败！\n"); return -1; }
    GTN_NetInit(NET_INIT_MODE_XML_STRICT, "C:\\Googol.xml", 60, &Status);
    GTN_ClrSts(CORE, AXIS, 1);
    GTN_ZeroPos(CORE, AXIS, 1);
    GTN_AxisOn(CORE, AXIS);
    Sleep(200);

    // 迭代学习主循环
    for (int i = 0; i < NUM_POSITIONS; i++) {
        double p_base = P_train_pulse[i];
        printf("\n[位置 %d/%d] 正在寻位至: %.2f 脉冲...\n", i+1, NUM_POSITIONS, p_base);
        move_to_position(p_base);

        double theta_j = 0.0; 

        for (int j = 0; j < NUM_TRIALS; j++) {
            // 1. 将当前计算的前馈注入电流环（转换成short类型的脉冲力矩）
            short acc_ff_signal = (short)(theta_j * ERROR_GAIN * 1000.0);
            GTN_SetFrictionCompensateValue(CORE, AXIS, 0, acc_ff_signal);

            // 2. 执行局部测试获取误差
            double e_j = execute_trial_and_get_error(p_base);

            // 3. ILC 核心更新公式: \theta_{j+1} = L * e_j + Q * \theta_j
            theta_j = ILC_L * e_j + ILC_Q * theta_j;

            printf("  Trial %d/%d: 剩余平均误差 = %.2f, 参数 theta = %.4f\n", j+1, NUM_TRIALS, e_j, theta_j);
        }

        theta_train[i] = theta_j; 
        printf("-> 位置 %.2f 学习完毕。最终参数 = %.4f\n", P_train_norm[i], theta_j);
        
        GTN_SetFrictionCompensateValue(CORE, AXIS, 0, 0); // 清除前馈
    }

    // 任务结束，平稳回零
    printf("\n正在回零...\n");
    move_to_position(0.0);
    GTN_AxisOff(CORE, AXIS);
    GTN_Close();

    // 导出 CSV 文件供 MATLAB 离线拟合使用
    FILE* fp = fopen("ilc_train_data.csv", "w");
    if (fp != NULL) {
        fprintf(fp, "P_train,theta_train\n");
        for (int i = 0; i < NUM_POSITIONS; i++) {
            fprintf(fp, "%.4f,%.6f\n", P_train_norm[i], theta_train[i]);
        }
        fclose(fp);
        printf("[成功] 数据已保存至 ilc_train_data.csv！\n");
    }

    timeEndPeriod(1);
    return 0;
}