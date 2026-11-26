/*==========================================================================
 * 纯 PID 控制 - 真实机床对比实验测试程序 (完全无前馈补偿版)
 *==========================================================================*/
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <Windows.h>
#include <mmsystem.h>
#include "gxn.h"

#pragma comment(lib, "winmm.lib")

#define CORE 2
#define AXIS 1

 /* --- 轨迹参数 (与采集/补偿代码完全对齐：双频混合激励测试轨迹) --- */
#define AMP_1           100000.0  // 低频幅值 (脉冲数)
#define FREQ_1_HZ       0.5       // 低频频率 (Hz)
#define AMP_2           50000.0   // 高频幅值 (脉冲数)
#define FREQ_2_HZ       1.25      // 高频频率 (Hz)

#define RUN_TIME_SEC    10.0      // 轨迹时长 (秒)
#define TIME_STEP       1         // 1ms 控制周期 (dt = 0.001s)
#define TOTAL_SEG       (int)((RUN_TIME_SEC * 1000.0) / TIME_STEP)
#define PI              3.14159265358979323846

#define FIFO0_SIZE      64
#define PT_MODE_DYNAMIC 1

#ifndef PT_SEG_NORMAL
#define PT_SEG_NORMAL   0
#endif
#ifndef PT_SEG_STOP
#define PT_SEG_STOP     2
#endif

/* 预计算轨迹缓冲指针 */
double* q_ref, * v_ref, * a_ref;

/* 预计算双频混合激励轨迹(带内存防空指针安全校验) */
int init_trajectory() {
    q_ref = (double*)malloc((TOTAL_SEG + 1) * sizeof(double));
    v_ref = (double*)malloc((TOTAL_SEG + 1) * sizeof(double));
    a_ref = (double*)malloc((TOTAL_SEG + 1) * sizeof(double));

    if (q_ref == NULL || v_ref == NULL || a_ref == NULL) {
        printf("安全错误：轨迹缓冲区内存分配失败！\n");
        if (q_ref != NULL) free(q_ref);
        if (v_ref != NULL) free(v_ref);
        if (a_ref != NULL) free(a_ref);
        return -1;
    }

    for (int i = 0; i <= TOTAL_SEG; i++) {
        double t = (double)(i * TIME_STEP) / 1000.0; // 转换为秒

        // 1. 位置轨迹：q(t) = A1*(1-cos(w1*t)) + A2*(1-cos(w2*t))
        q_ref[i] = AMP_1 * (1.0 - cos(2.0 * PI * FREQ_1_HZ * t)) +
            AMP_2 * (1.0 - cos(2.0 * PI * FREQ_2_HZ * t));

        // 2. 速度轨迹
        v_ref[i] = AMP_1 * (2.0 * PI * FREQ_1_HZ) * sin(2.0 * PI * FREQ_1_HZ * t) +
            AMP_2 * (2.0 * PI * FREQ_2_HZ) * sin(2.0 * PI * FREQ_2_HZ * t);

        // 3. 加速度轨迹
        a_ref[i] = AMP_1 * pow(2.0 * PI * FREQ_1_HZ, 2) * cos(2.0 * PI * FREQ_1_HZ * t) +
            AMP_2 * pow(2.0 * PI * FREQ_2_HZ, 2) * cos(2.0 * PI * FREQ_2_HZ * t);
    }
    return 0;
}

int main() {
    long Status;
    long mask = 1L << (AXIS - 1);
    short space = 0;
    int started = 0, filled = 0;
    long prfSts = 0;
    double axisError = 0.0;

    // 强制提升 Windows 线程睡眠精度至 1ms
    timeBeginPeriod(1);

    printf("==================================================\n");
    printf("  纯 PID 控制 - 实机无补偿对比实验及数据存盘程序\n");
    printf("==================================================\n");

    /* 1. 初始化轨迹 */
    if (init_trajectory() != 0) return -1;
    printf("双频混合激励测试轨迹已预计算完成 (时长: %.1f 秒).\n", RUN_TIME_SEC);

    /* 2. 创建实验结果保存文件 */
    FILE* fp_log = fopen("real_experiment_result.csv", "w");
    if (fp_log != NULL) {
        fprintf(fp_log, "Time_ms,Pos_Ref,Vel_Ref,Axis_Error\n"); // 写入 CSV 表头
    }
    else {
        printf("警告：无法创建数据存盘文件！\n");
    }

    /* 3. 板卡与总线网络使能流 */
    GTN_OpenCard();
    GTN_NetInit(NET_INIT_MODE_XML_STRICT, "C:\\Users\\admin\\Desktop\\sine_compensate\\Googol.xml", 60, &Status);
    GTN_ClrSts(CORE, AXIS, 1);
    GTN_ZeroPos(CORE, AXIS, 1);
    GTN_AxisOn(CORE, AXIS); // 闭合电机使能
    Sleep(200);

    /* 4. 配置 PT 动态模式缓冲区 */
    GTN_PrfPt(CORE, AXIS, PT_MODE_DYNAMIC);
    GTN_SetPtMemory(CORE, AXIS, 0);
    GTN_PtClear(CORE, AXIS, 0);

    printf("控制卡通信正常，开始下发轨迹 (当前为纯 PID 裸跑模式)...\n");

    /* 5. 运动与数据采集主循环 */
    for (int i = 1; i <= TOTAL_SEG; i++) {
        short segType = (i == TOTAL_SEG) ? PT_SEG_STOP : PT_SEG_NORMAL;
        long endTime = i * TIME_STEP;

        while (1) {
            // 查询当前硬件 FIFO 空间
            GTN_PtSpace(CORE, AXIS, &space, 0);
            if (space > 0) break; // 空间可用，跳出死循环执行下发

            // 缓冲区装满后，命令控制卡正式起跑轨迹
            if (!started && filled >= FIFO0_SIZE) {
                GTN_PtStart(CORE, mask, 0);
                started = 1;
            }

            // 只要电机开始运行，在等待空闲的 1ms 间隙内抓取纯 PID 的跟踪误差
            if (started) {
                double q = q_ref[i];
                double v = v_ref[i];

                // 【核心区别】：完全关闭前馈通道，下发 0.0，不实施任何摩擦力补偿
                GTN_SetFrictionCompensateValue(CORE, AXIS, 0.0, 0.0);

                // 读取此时纯 PID 控制下的实时跟踪误差并录入文件
                GTN_GetAxisError(CORE, AXIS, &axisError, 1, NULL);
                if (fp_log != NULL) {
                    fprintf(fp_log, "%d,%.2f,%.2f,%.2f\n", i * TIME_STEP, q, v, axisError);
                }
            }
            Sleep(1);
        }

        // 下发位置点到控制卡缓冲区
        GTN_PtData(CORE, AXIS, q_ref[i], endTime, segType);
        filled++;
    }

    // 安全防御：如果点数不足也强行启动
    if (!started && filled > 0) {
        GTN_PtStart(CORE, mask, 0);
        started = 1;
    }

    /* 6. 等待运动完全结束 */
    while (1) {
        GTN_GetPrfSts(CORE, AXIS, &prfSts, 1, NULL);
        if ((prfSts & (1L << 10)) == 0) break; // 运动结束

        // 结束前的滑动期持续维持纯 PID 数据落盘
        GTN_GetAxisError(CORE, AXIS, &axisError, 1, NULL);
        if (fp_log != NULL) {
            fprintf(fp_log, "%d,%.2f,%.2f,%.2f\n", TOTAL_SEG * TIME_STEP, q_ref[TOTAL_SEG], v_ref[TOTAL_SEG], axisError);
        }
        Sleep(1);
    }

    /* 7. 实验安全收尾 */
    printf("\n测试完成！正在安全切断通道与保存结果...\n");
    GTN_SetFrictionCompensateValue(CORE, AXIS, 0.0, 0.0);

    if (fp_log != NULL) {
        fclose(fp_log);
        printf("纯 PID 跟踪实验数据已成功保存至当前目录下 real_experiment_result.csv 中！\n");
    }

    GTN_AxisOff(CORE, AXIS);
    GTN_Close();
    free(q_ref); free(v_ref); free(a_ref);
    timeEndPeriod(1);

    return 0;
}