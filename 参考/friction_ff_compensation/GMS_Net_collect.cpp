/*==============================================================
 * 机床摩擦力特性 - 高频数据采集程序
 * 专为 GMS-Net / PINN 离线训练设计
 * 基于固高 GTN 控制器 PT 动态模式
 *==============================================================*/
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <Windows.h>
#include <mmsystem.h> // 需要链接 winmm.lib 用于 timeBeginPeriod
#include "gxn.h"

#pragma comment(lib, "winmm.lib")

 /*==============================================================
  * 控制卡与轴配置
  *==============================================================*/
#define CORE    2
#define AXIS    1

  /*==============================================================
   * 双频混合激励轨迹参数 (用于充分激发高低速摩擦特性)
   * 请根据你的机床脉冲当量(Pulse/mm)自行放大或缩小以下幅值
   *==============================================================*/
#define AMP_1           100000.0  // 低频幅值 (脉冲数)
#define FREQ_1_HZ       0.5       // 低频频率 (Hz)
#define AMP_2           50000.0   // 高频幅值 (脉冲数)
#define FREQ_2_HZ       1.25      // 高频频率 (Hz)

#define RUN_TIME_SEC    60.0      // 采集总时长 (秒)
#define TIME_STEP       1         // PT 步长 (ms)

   /*==============================================================
    * PT参数
    *==============================================================*/
#define FIFO0_SIZE      64
#define PT_MODE_DYNAMIC 1
#define PT_SEG_NORMAL   0
#define PT_SEG_STOP     2
#define PI 3.14159265358979323846
#define TOTAL_SEG       (int)((RUN_TIME_SEC * 1000.0) / TIME_STEP)

    /*==============================================================
     * 数据记录参数与结构体
     *==============================================================*/
#define LOG_CAPACITY    (TOTAL_SEG + 2000)

typedef struct {
    double time_ms;       // 绝对时间戳
    double prf_pos;       // 规划位置 (脉冲)
    double enc_pos;       // 实际位置 (脉冲)
    double enc_vel;       // 实际速度 (脉冲/ms)
    double track_error;   // 跟踪误差 (脉冲)
    short  ctrl_output;   // 控制力矩/DAC输出 (关键：用于推导摩擦力)
} DataRecord;

typedef struct {
    DataRecord* records;
    int capacity;
    int count;
} DataLogger;

/* 全局变量 */
LARGE_INTEGER g_freq, g_startTime;
DataLogger g_logger;
double* g_desiredPos = NULL;

/* 获取毫秒级高精度时间 */
double getElapsedMs() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - g_startTime.QuadPart) * 1000.0 / (double)g_freq.QuadPart;
}

/* 预计算双频混合激励轨迹 */
int precomputeTrajectory() {
    g_desiredPos = (double*)malloc((TOTAL_SEG + 1) * sizeof(double));
    if (g_desiredPos == NULL) return -1;

    for (int i = 0; i <= TOTAL_SEG; i++) {
        double t = (double)(i * TIME_STEP) / 1000.0; // 转换为秒
        // 双频混合位置轨迹公式: q(t) = A1(1-cos(w1*t)) + A2(1-cos(w2*t))
        g_desiredPos[i] = AMP_1 * (1.0 - cos(2.0 * PI * FREQ_1_HZ * t)) +
            AMP_2 * (1.0 - cos(2.0 * PI * FREQ_2_HZ * t));
    }
    return 0;
}

/* 数据采集核心函数 */
/* 数据采集核心函数 */
void log_current_state(DataLogger* logger) {
    if (logger->count >= logger->capacity) return;

    DataRecord* rec = &logger->records[logger->count];

    // 【警告修复】: 绝对时间戳非常重要！你之前把它注释掉了，
    // 这会导致 MATLAB 里的 dt 混乱，必须打开！
    rec->time_ms = getElapsedMs();

    // 获取机床当前核心状态
    GTN_GetPrfPos(CORE, AXIS, &rec->prf_pos, 1, NULL);
    GTN_GetEncPos(CORE, AXIS, &rec->enc_pos, 1, NULL);
    GTN_GetEncVel(CORE, AXIS, &rec->enc_vel, 1, NULL);
    GTN_GetAxisError(CORE, AXIS, &rec->track_error, 1, NULL);

    // 【核心修复】: 替换为截图中的 GTN_GetAtlTorque API
    // 注意：根据截图原型，它只有 3 个参数，没有后面的 1, NULL
    GTN_GetAtlTorque(CORE, AXIS, &rec->ctrl_output);

    logger->count++;
}

/* 导出为 CSV，供 MATLAB 训练使用 */
void logger_save_to_file(DataLogger* logger, const char* filename) {
    FILE* fp = fopen(filename, "w");
    if (fp == NULL) return;

    fprintf(fp, "time_ms,prf_pos,enc_pos,enc_vel,track_error,ctrl_output\n");
    for (int i = 0; i < logger->count; i++) {
        DataRecord* rec = &logger->records[i];
        fprintf(fp, "%.4f,%.2f,%.2f,%.4f,%.2f,%d\n",
            rec->time_ms, rec->prf_pos, rec->enc_pos,
            rec->enc_vel, rec->track_error, rec->ctrl_output);
    }
    fclose(fp);
    printf("成功导出训练数据至: %s (共 %d 行)\n", filename, logger->count);
}

int main() {
    short ret;
    long Status;
    long mask = 1L << (AXIS - 1);
    short space = 0;
    int started = 0, filled = 0;
    long prfSts = 0;

    // 强制提升 Windows 线程睡眠精度至 1ms
    timeBeginPeriod(1);

    printf("============================================\n");
    printf("  GMS-Net 机床训练数据高频采集程序\n");
    printf("============================================\n");

    /* 1. 初始化内存与轨迹 */
    g_logger.records = (DataRecord*)malloc(LOG_CAPACITY * sizeof(DataRecord));
    g_logger.capacity = LOG_CAPACITY;
    g_logger.count = 0;

    if (precomputeTrajectory() != 0) return -1;
    printf("双频混合激励轨迹已生成 (时长: %.1f 秒).\n", RUN_TIME_SEC);

    /* 2. 初始化控制卡 (省略异常处理代码保持精简) */
    QueryPerformanceFrequency(&g_freq);
    GTN_OpenCard();
    GTN_NetInit(NET_INIT_MODE_XML_STRICT, "C:\\Users\\admin\\Desktop\\sine_compensate\\Googol.xml", 60, &Status);
    GTN_ClrSts(CORE, AXIS, 1);
    GTN_ZeroPos(CORE, AXIS, 1);
    GTN_AxisOn(CORE, AXIS);
    Sleep(200);

    /* 3. 准备 PT 模式 */
    GTN_PrfPt(CORE, AXIS, PT_MODE_DYNAMIC);
    GTN_SetPtMemory(CORE, AXIS, 0);
    GTN_PtClear(CORE, AXIS, 0);

    printf("开始运行轨迹并采集数据...\n");

    /* 4. 运动与高频采集主循环 */
    for (int i = 1; i <= TOTAL_SEG; i++) {
        short segType = (i == TOTAL_SEG) ? PT_SEG_STOP : PT_SEG_NORMAL;
        long endTime = i * TIME_STEP;

        while (1) {
            GTN_PtSpace(CORE, AXIS, &space, 0);
            if (space > 0) break;

            if (!started && filled >= FIFO0_SIZE) {
                GTN_PtStart(CORE, mask, 0);
                QueryPerformanceCounter(&g_startTime);
                started = 1;
            }

            // 只要开始运动，就在等待 FIFO 空间的间隙高频采集数据
            if (started) {
                log_current_state(&g_logger);
            }
            Sleep(1); // 1ms 休眠
        }

        GTN_PtData(CORE, AXIS, g_desiredPos[i], endTime, segType);
        filled++;
    }

    if (!started && filled > 0) {
        GTN_PtStart(CORE, mask, 0);
        QueryPerformanceCounter(&g_startTime);
        started = 1;
    }

    /* 5. 等待运动结束，期间持续采集 */
    while (1) {
        GTN_GetPrfSts(CORE, AXIS, &prfSts, 1, NULL);
        if ((prfSts & (1L << 10)) == 0) break; // 运动结束

        log_current_state(&g_logger);
        Sleep(1);
    }

    /* 6. 保存数据并清理 */
    printf("\n运动完成！\n");
    logger_save_to_file(&g_logger, "gms_training_data.csv");

    GTN_AxisOff(CORE, AXIS);
    GTN_Close();
    free(g_desiredPos);
    free(g_logger.records);
    timeEndPeriod(1);

    return 0;
}