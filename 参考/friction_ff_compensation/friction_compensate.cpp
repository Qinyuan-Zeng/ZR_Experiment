#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <Windows.h>
#include <conio.h>
#include <vector>
#include "gxn.h"

// ================= 参数配置 =================
#define CORE            2
#define AXIS            1
#define PI              3.14159265358979323846

const double SINE_FREQ = 0.25;
const double SINE_AMP = 131072.0;
const int    CYCLES = 40;
const double TIME_STEP = 10.0; // 10ms 步长
const double TOTAL_TIME = (1.0 / SINE_FREQ) * 1000.0 * CYCLES; // 160,000 ms
const int    TOTAL_POINTS = (int)(TOTAL_TIME / TIME_STEP);

#define RBF_NODES_POS       5
#define RBF_NODES_VEL       15
#define RBF_TOTAL_NODES     75
#define POS_MIN             -140000.0
#define POS_MAX              140000.0
#define VEL_MIN             -300.0
#define VEL_MAX              300.0
#define LEARNING_RATE        0.0001
#define WEIGHT_DECAY         0.00002
#define MAX_WEIGHT           500.0

const double ALPHA_ERR = 0.05;
const double ALPHA_FF = 0.1;
const double ERROR_DEADZONE = 5.0;

// ================= 结构体定义 =================
typedef struct { double pos_center; double vel_center; } RBFCenter;
typedef struct {
    RBFCenter centers[RBF_TOTAL_NODES];
    double weights[RBF_TOTAL_NODES];
    double sigma_pos, sigma_vel, phi[RBF_TOTAL_NODES];
} RBFNetwork;

typedef struct {
    double time_ms, position, velocity, tracking_error, ff_output;
    double weights[RBF_TOTAL_NODES];
} WeightRecord;

typedef struct {
    WeightRecord* records;
    int capacity, count, sample_interval, sample_counter;
} WeightLogger;

RBFNetwork g_net;
WeightLogger g_log;

// ================= 核心算法与辅助函数 =================
double normalize_pos(double p) { return (p - POS_MIN) / (POS_MAX - POS_MIN); }
double normalize_vel(double v) { return (v - VEL_MIN) / (VEL_MAX - VEL_MIN); }

double calculate_weight_norm() {
    double sum = 0;
    for (int i = 0; i < RBF_TOTAL_NODES; i++) sum += g_net.weights[i] * g_net.weights[i];
    return sqrt(sum);
}

void rbf_init() {
    int idx = 0;
    for (int i = 0; i < RBF_NODES_POS; i++) {
        for (int j = 0; j < RBF_NODES_VEL; j++) {
            g_net.centers[idx].pos_center = i * (1.0 / (RBF_NODES_POS - 1));
            g_net.centers[idx].vel_center = j * (1.0 / (RBF_NODES_VEL - 1));
            g_net.weights[idx++] = 0.0;
        }
    }
    g_net.sigma_pos = 0.8 / (RBF_NODES_POS - 1);
    g_net.sigma_vel = 0.8 / (RBF_NODES_VEL - 1);
}

double rbf_compute(double p, double v) {
    double pn = normalize_pos(p), vn = normalize_vel(v), out = 0.0;
    for (int i = 0; i < RBF_TOTAL_NODES; i++) {
        double dp = (pn - g_net.centers[i].pos_center) / g_net.sigma_pos;
        double dv = (vn - g_net.centers[i].vel_center) / g_net.sigma_vel;
        g_net.phi[i] = exp(-0.5 * (dp * dp + dv * dv));
        out += g_net.weights[i] * g_net.phi[i];
    }
    return out;
}

void rbf_update(double err) {
    for (int i = 0; i < RBF_TOTAL_NODES; i++) {
        g_net.weights[i] += LEARNING_RATE * err * g_net.phi[i] - WEIGHT_DECAY * g_net.weights[i];
        if (g_net.weights[i] > MAX_WEIGHT) g_net.weights[i] = MAX_WEIGHT;
        if (g_net.weights[i] < -MAX_WEIGHT) g_net.weights[i] = -MAX_WEIGHT;
    }
}

// 保存所有数据到 CSV
void logger_save_complete(WeightLogger* l) {
    if (l->count == 0) {
        printf("\n[警告] 没有记录到任何数据，跳过保存。\n");
        return;
    }
    FILE* fp = fopen("rbf_learning_full.csv", "w");
    if (fp) {
        // 1. 写入表头 (对齐之前提供的 MATLAB 脚本变量名)
        fprintf(fp, "time_ms,pos,vel,err,ff");
        for (int j = 0; j < RBF_TOTAL_NODES; j++) fprintf(fp, ",w%d", j);
        fprintf(fp, "\n");

        // 2. 写入数据行
        for (int i = 0; i < l->count; i++) {
            WeightRecord* r = &l->records[i];
            fprintf(fp, "%.2f,%.2f,%.2f,%.4f,%.4f",
                r->time_ms, r->position, r->velocity, r->tracking_error, r->ff_output);
            for (int j = 0; j < RBF_TOTAL_NODES; j++) fprintf(fp, ",%.6f", r->weights[j]);
            fprintf(fp, "\n");
        }
        fclose(fp);
        printf("\n[完成] 成功保存 %d 条记录至 rbf_learning_full.csv\n", l->count);
    }
    else {
        printf("\n[错误] 无法创建 CSV 文件，请检查磁盘权限！\n");
    }
}

// ================= 主程序 =================
int main() {
    short ret; long status, sts;
    system("cls");
    printf("============================================================\n");
    printf("        GTN RBF 在线确定学习 - 自动数据保存版\n");
    printf("============================================================\n\n");

    rbf_init();
    // 初始化记录器：容量 20000 条，每 10ms 记录一次，可覆盖 200 秒
    g_log.capacity = 20000;
    g_log.records = (WeightRecord*)malloc(g_log.capacity * sizeof(WeightRecord));
    g_log.count = 0; g_log.sample_interval = 1; // 采样间隔设为1表示每次循环都检查采样
    g_log.sample_counter = 0;

    LARGE_INTEGER freq, startT;
    QueryPerformanceFrequency(&freq);

    printf("[1/5] 正在打开控制卡...");
    ret = GTN_OpenCard();
    if (ret != 0) { printf("\n[错误] 失败 (ret=%d)\n", ret); return -1; }
    printf(" 成功\n");

    printf("[2/5] 正在初始化网络...");
    ret = GTN_NetInit(NET_INIT_MODE_XML_STRICT, "C:\\Users\\MCLab\\Desktop\\MotionStudio_x64_V2.3.14.0_develop\\Googol.xml", 60, &status);
    if (ret != 0) { printf("\n[错误] 失败 (ret=%d)\n", ret); return -1; }
    printf(" 成功\n");

    GTN_ClrSts(CORE, AXIS, 1);
    GTN_AxisOn(CORE, AXIS);
    GTN_ZeroPos(CORE, AXIS);
    printf("[3/5] 轴 %d 使能成功\n", AXIS);

    GTN_PrfPt(CORE, AXIS, 1);
    GTN_PtClear(CORE, AXIS, 0);
    printf("[4/5] PT 模式准备就绪\n");

    double omega = 2.0 * PI * SINE_FREQ;
    int ptsSent = 0;
    short space = 0;
    bool started = false;
    double f_err = 0.0, f_ff = 0.0, elapsed = 0.0;
    int currentCycle = 0;

    printf("[5/5] 开始运动与学习 (160秒)...\n");
    printf("------------------------------------------------------------\n");
    printf("%-10s | %-10s | %-8s | %-8s | %-8s\n", "Time(ms)", "Cycle", "Error", "FF_Out", "W_Norm");
    printf("------------------------------------------------------------\n");

    while (true) {
        GTN_GetSts(CORE, AXIS, &sts);

        // 填充 FIFO
        GTN_PtSpace(CORE, AXIS, &space, 0);
        while (space > 0 && ptsSent <= TOTAL_POINTS) {
            double t_ms = ptsSent * TIME_STEP;
            double p = SINE_AMP * sin(omega * t_ms / 1000.0);
            GTN_PtData(CORE, AXIS, p, (long)t_ms, (ptsSent == TOTAL_POINTS) ? 2 : 0);
            ptsSent++;
            space--;
        }

        if (!started && ptsSent > 50) {
            GTN_PtStart(CORE, 1 << (AXIS - 1), 0);
            QueryPerformanceCounter(&startT);
            started = true;
        }

        if (started) {
            LARGE_INTEGER now; QueryPerformanceCounter(&now);
            elapsed = (double)(now.QuadPart - startT.QuadPart) * 1000.0 / (double)freq.QuadPart;

            if (elapsed < TOTAL_TIME) {
                double err, dP, dV;
                GTN_GetAxisError(CORE, AXIS, &err, 1, NULL);

                double t_s = elapsed / 1000.0;
                dP = SINE_AMP * sin(omega * t_s);
                dV = (SINE_AMP * omega * cos(omega * t_s)) / 1000.0;

                f_err = ALPHA_ERR * err + (1.0 - ALPHA_ERR) * f_err;
                double eff_err = (fabs(f_err) < ERROR_DEADZONE) ? 0.0 : f_err;

                double rawFF = rbf_compute(dP, dV);
                f_ff = ALPHA_FF * rawFF + (1.0 - ALPHA_FF) * f_ff;

                GTN_SetFrictionCompensateValue(CORE, AXIS, 0.0, f_ff);
                rbf_update(eff_err);

                // 数据采样记录 (每 10ms 存一条)
                static double lastRecordTime = -10.0;
                if (elapsed - lastRecordTime >= 10.0 && g_log.count < g_log.capacity) {
                    WeightRecord* r = &g_log.records[g_log.count++];
                    r->time_ms = elapsed; r->position = dP; r->velocity = dV;
                    r->tracking_error = err; r->ff_output = f_ff;
                    for (int j = 0; j < RBF_TOTAL_NODES; j++) r->weights[j] = g_net.weights[j];
                    lastRecordTime = elapsed;
                }

                // 周期统计输出
                int cycleIdx = (int)(elapsed / (1000.0 / SINE_FREQ)) + 1;
                if (cycleIdx > currentCycle) {
                    if (currentCycle > 0) printf("\n[汇总] 周期 %2d 完成 | 误差: %6.2f | 权重能级: %.4f\n", currentCycle, err, calculate_weight_norm());
                    currentCycle = cycleIdx;
                }

                // 实时行刷新
                if ((int)elapsed % 100 < 15) {
                    printf("\r%-10.0f | %2d/%-7d | %8.2f | %8.2f | %8.4f",
                        elapsed, currentCycle, CYCLES, err, f_ff, calculate_weight_norm());
                }
            }
            else { break; }
        }
        if (ptsSent > TOTAL_POINTS && !(sts & 0x400)) break;
        Sleep(1);
    }

    printf("\n\n------------------------------------------------------------\n");
    printf("运动结束。正在执行数据固化...");

    // 关键：保存到磁盘
    logger_save_complete(&g_log);

    GTN_SetFrictionCompensateValue(CORE, AXIS, 0.0, 0.0);
    GTN_AxisOff(CORE, AXIS);
    GTN_Close();

    printf("系统已安全关闭。按任意键退出。\n");
    free(g_log.records);
    _getch();
    return 0;
}