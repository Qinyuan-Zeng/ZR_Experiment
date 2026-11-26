/*==============================================================
 * ILCBF-GPPDF 第三阶段：连续高斯过程(GP)前馈实时注入程序（修正版）
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

#define CORE          2
#define AXIS          1
#define TIME_STEP     1 
#define GP_TABLE_SIZE 101

double g_gp_theta_table[GP_TABLE_SIZE]; 

typedef struct {
    double pos; 
    double vel; 
    double acc; 
} TrajectoryPoint;

TrajectoryPoint* g_traj = NULL;
int g_total_segments = 0;
double g_max_stroke = 100000.0; // 对应归一化行程 1.0 的脉冲总数

/*==============================================================
 * 辅助函数：模拟加载 MATLAB fitrgp 预测出来的平滑曲线
 *==============================================================*/
void load_gp_model() {
    for (int i = 0; i < GP_TABLE_SIZE; i++) {
        double rho = (double)i / (GP_TABLE_SIZE - 1); 
        double m_bar = 1.0; 
        g_gp_theta_table[i] = m_bar * (1.0 - 2.0 * pow(rho - 0.5, 2)); 
    }
    printf("GP 连续参数查表已在内存中建立.\n");
}

/*==============================================================
 * 核心查表函数：根据机床当前实时脉冲坐标，线性插值获得参数 \theta
 *==============================================================*/
double interpolate_gp_theta(double current_pos) {
    double rho = current_pos / g_max_stroke;
    if (rho < 0.0) rho = 0.0;
    if (rho > 1.0) rho = 1.0;

    double exact_idx = rho * (GP_TABLE_SIZE - 1);
    int idx_low = (int)floor(exact_idx);
    int idx_high = (int)ceil(exact_idx);
    
    if (idx_low == idx_high) return g_gp_theta_table[idx_low];
    
    double weight = exact_idx - idx_low;
    return g_gp_theta_table[idx_low] * (1.0 - weight) + g_gp_theta_table[idx_high] * weight;
}

/* 生成主运行轨迹及加速度参数 */
void precompute_trajectory(double run_time) {
    g_total_segments = (int)(run_time * 1000.0 / TIME_STEP);
    g_traj = (TrajectoryPoint*)malloc((g_total_segments + 1) * sizeof(TrajectoryPoint));

    double amp = 50000.0;
    double freq = 0.5;
    double PI = 3.14159265358979;

    for (int i = 0; i <= g_total_segments; i++) {
        double t = i * 0.001; 
        g_traj[i].pos = amp * (1.0 - cos(2.0 * PI * freq * t));
        g_traj[i].vel = amp * 2.0 * PI * freq * sin(2.0 * PI * freq * t);
        g_traj[i].acc = amp * pow(2.0 * PI * freq, 2) * cos(2.0 * PI * freq * t);
    }
}

int main() {
    long Status;
    long mask = 1L << (AXIS - 1);
    short space = 0;
    long prfSts = 0;

    timeBeginPeriod(1);

    printf("============================================\n");
    printf("  第三阶段：机床 GPPDF 在线连续前馈实时注入（修正版）\n");
    printf("============================================\n");

    load_gp_model();
    precompute_trajectory(10.0); // 10秒复杂轨迹

    // 初始化控制卡
    if (GTN_OpenCard() != 0) { printf("开卡失败！\n"); return -1; }
    GTN_NetInit(NET_INIT_MODE_XML_STRICT, "C:\\Googol.xml", 60, &Status);
    GTN_ClrSts(CORE, AXIS, 1);
    GTN_ZeroPos(CORE, AXIS, 1);
    GTN_AxisOn(CORE, AXIS);
    Sleep(200);

    // 准备 PT 运动模式
    GTN_PrfPt(CORE, AXIS, 1); // 1 代表动态 PT 模式
    GTN_SetPtMemory(CORE, AXIS, 0);
    GTN_PtClear(CORE, AXIS, 0); // 【修正】增加了管道号参数 0

    printf("主轨迹运行中，前馈补偿实时打入电流环...\n");

    int started = 0;
    int filled = 0;

    for (int i = 1; i <= g_total_segments; i++) {
        short segType = (i == g_total_segments) ? 2 : 0; // 2:PT_SEG_STOP, 0:PT_SEG_NORMAL
        long endTime = i * TIME_STEP;

        while (1) {
            GTN_PtSpace(CORE, AXIS, &space, 0); // 【修正】增加了管道号参数 0
            if (space > 0) break;

            if (!started && filled >= 64) {
                GTN_PtStart(CORE, mask, 0); // 【修正】增加了管道号参数 0
                started = 1;
            }

            // ========================================================
            // 高频实时注入核心逻辑（在等待底层 FIFO 空闲的 1ms 内高频发生）
            // ========================================================
            if (started) {
                double current_prf_pos;
                // 1. 读取当前瞬时的物理规划位置
                GTN_GetPrfPos(CORE, AXIS, &current_prf_pos, 1, NULL);
                
                // 2. 根据该位置插值提取高斯过程回归计算出的 \theta
                double theta_p = interpolate_gp_theta(current_prf_pos);
                
                // 3. 计算前馈量: u_ff = \theta(p) * a_d * 物理增益
                double torque_gain = 0.05; 
                short acc_ff_value = (short)(theta_p * g_traj[i].acc * torque_gain);

                // 4. 打入电流环：vel_ff 传 0，acc_ff 传计算值
                GTN_SetFrictionCompensateValue(CORE, AXIS, 0, acc_ff_value);
            }
            // ========================================================

            Sleep(1);
        }

        // 送入 PT 缓存区 【修正】增加了第6个参数：管道号 0
        GTN_PtData(CORE, AXIS, g_traj[i].pos, endTime, segType, 0);
        filled++;
    }

    if (!started) GTN_PtStart(CORE, mask, 0); // 【修正】管道号 0

    // 等待 PT 运动完全停稳，期间保持末态补偿
    while (1) {
        GTN_GetPrfSts(CORE, AXIS, &prfSts, 1, NULL);
        if ((prfSts & (1L << 10)) == 0) { 
            // 运动完全结束，【必须】清除前馈，防止静止发热
            GTN_SetFrictionCompensateValue(CORE, AXIS, 0, 0);
            break; 
        }
        
        double current_prf_pos;
        GTN_GetPrfPos(CORE, AXIS, &current_prf_pos, 1, NULL);
        double theta_p = interpolate_gp_theta(current_prf_pos);
        short acc_ff_value = (short)(theta_p * g_traj[g_total_segments].acc * 0.05);
        GTN_SetFrictionCompensateValue(CORE, AXIS, 0, acc_ff_value);

        Sleep(1);
    }

    printf("\nGPPDF 补偿轨迹运行完成！\n");

    GTN_AxisOff(CORE, AXIS);
    GTN_Close();
    free(g_traj);
    timeEndPeriod(1);

    return 0;
}