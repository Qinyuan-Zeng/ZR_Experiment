/*==============================================================
 * RBF在线确定学习 - 速度环前馈补偿
 * 基于PT动态模式，使用RBF神经网络在线学习
 *==============================================================*/
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <Windows.h>
#include "gxn.h"

 /*==============================================================
  * 控制卡配置
  *==============================================================*/
#define CORE    2
#define AXIS    1

  /*==============================================================
   * 正弦轨迹参数
   *==============================================================*/
#define SINE_AMPLITUDE  131072.0
#define SINE_PERIOD     4000        
#define SINE_CYCLES     8
#define TIME_STEP       1        

   /*==============================================================
    * PT参数
    *==============================================================*/
#define FIFO0_SIZE      64
#define FIFO1_SIZE      64
#define PT_MODE_DYNAMIC     1
#define PT_SEG_NORMAL       0
#define PT_SEG_STOP         2

#define PI 3.14159265358979323846

#define SEG_PER_CYCLE   (SINE_PERIOD / TIME_STEP)
#define TOTAL_SEG       (SEG_PER_CYCLE * SINE_CYCLES)

    /*==============================================================
     * RBF网络参数
     *==============================================================*/
#define RBF_NODES_POS       5
#define RBF_NODES_VEL       5
#define RBF_TOTAL_NODES     (RBF_NODES_POS * RBF_NODES_VEL)

#define POS_MIN             -140000.0
#define POS_MAX             140000.0
#define VEL_MIN             -300.0
#define VEL_MAX             300.0

#define SIGMA_SCALE         0.8
#define RBF_SIGMA_POS       (1.0 / (RBF_NODES_POS - 1) * SIGMA_SCALE)
#define RBF_SIGMA_VEL       (1.0 / (RBF_NODES_VEL - 1) * SIGMA_SCALE)

#define LEARNING_RATE       0.0001
#define WEIGHT_DECAY        0.00002
#define MAX_WEIGHT          500.0

     /*==============================================================
      * 安全限制
      *==============================================================*/
#define FF_OUTPUT_MAX       100.0
#define FF_OUTPUT_MIN       -100.0
#define ERROR_THRESHOLD     20000.0

      /*==============================================================
       * 记录参数
       *==============================================================*/
#define LOG_SAMPLE_INTERVAL 10
#define LOG_CAPACITY        10000

       /*==============================================================
        * RBF网络结构体
        *==============================================================*/
typedef struct {
    double pos_center;
    double vel_center;
} RBFCenter;

typedef struct {
    RBFCenter centers[RBF_TOTAL_NODES];
    double weights[RBF_TOTAL_NODES];
    double sigma_pos;
    double sigma_vel;
    double learning_rate;
    double phi[RBF_TOTAL_NODES];
} RBFNetwork;

typedef struct {
    double time_ms;
    double position;
    double velocity;
    double tracking_error;
    double ff_output;
    double weights[RBF_TOTAL_NODES];
} WeightRecord;

typedef struct {
    WeightRecord* records;
    int capacity;
    int count;
    int sample_interval;
    int sample_counter;
} WeightLogger;

/*==============================================================
 * 全局变量
 *==============================================================*/
LARGE_INTEGER g_freq, g_startTime;
RBFNetwork g_rbfNet;
WeightLogger g_logger;
double* g_desiredPos = NULL;
double* g_desiredVel = NULL;

/*==============================================================
 * 时间函数
 *==============================================================*/
double getElapsedMs()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - g_startTime.QuadPart) * 1000.0 / (double)g_freq.QuadPart;
}

/*==============================================================
 * 归一化函数
 *==============================================================*/
double normalize_pos(double pos)
{
    return (pos - POS_MIN) / (POS_MAX - POS_MIN);
}

double normalize_vel(double vel)
{
    return (vel - VEL_MIN) / (VEL_MAX - VEL_MIN);
}

/*==============================================================
 * RBF网络初始化
 *==============================================================*/
void rbf_init(RBFNetwork* net)
{
    int i, j, idx;
    double pos_step = 1.0 / (RBF_NODES_POS - 1);
    double vel_step = 1.0 / (RBF_NODES_VEL - 1);

    idx = 0;
    for (i = 0; i < RBF_NODES_POS; i++) {
        for (j = 0; j < RBF_NODES_VEL; j++) {
            net->centers[idx].pos_center = i * pos_step;
            net->centers[idx].vel_center = j * vel_step;
            net->weights[idx] = 0.0;
            idx++;
        }
    }

    net->sigma_pos = RBF_SIGMA_POS;
    net->sigma_vel = RBF_SIGMA_VEL;
    net->learning_rate = LEARNING_RATE;
    memset(net->phi, 0, sizeof(net->phi));
}

/*==============================================================
 * RBF前向计算
 *==============================================================*/
double rbf_compute_output(RBFNetwork* net, double pos, double vel)
{
    int i;
    double pos_norm = normalize_pos(pos);
    double vel_norm = normalize_vel(vel);
    double output = 0.0;

    for (i = 0; i < RBF_TOTAL_NODES; i++) {
        double d_pos = (pos_norm - net->centers[i].pos_center) / net->sigma_pos;
        double d_vel = (vel_norm - net->centers[i].vel_center) / net->sigma_vel;
        double dist_sq = d_pos * d_pos + d_vel * d_vel;
        net->phi[i] = exp(-0.5 * dist_sq);
    }

    for (i = 0; i < RBF_TOTAL_NODES; i++) {
        output += net->weights[i] * net->phi[i];
    }
    return output;
}

/*==============================================================
 * RBF权重更新
 *==============================================================*/
void rbf_update_weights(RBFNetwork* net, double error)
{
    int i;
    for (i = 0; i < RBF_TOTAL_NODES; i++) {
        double update = net->learning_rate * error * net->phi[i]
            - WEIGHT_DECAY * net->weights[i];
        net->weights[i] += update;

        if (net->weights[i] > MAX_WEIGHT) net->weights[i] = MAX_WEIGHT;
        if (net->weights[i] < -MAX_WEIGHT) net->weights[i] = -MAX_WEIGHT;
    }
}

/*==============================================================
 * 输出限幅
 *==============================================================*/
double clamp_output(double value, double min_val, double max_val)
{
    if (value > max_val) return max_val;
    if (value < min_val) return min_val;
    return value;
}

/*==============================================================
 * 记录器初始化
 *==============================================================*/
int logger_init(WeightLogger* logger, int capacity, int sample_interval)
{
    logger->records = (WeightRecord*)malloc(capacity * sizeof(WeightRecord));
    if (logger->records == NULL) return -1;
    logger->capacity = capacity;
    logger->count = 0;
    logger->sample_interval = sample_interval;
    logger->sample_counter = 0;
    return 0;
}

/*==============================================================
 * 记录器释放
 *==============================================================*/
void logger_free(WeightLogger* logger)
{
    if (logger->records != NULL) {
        free(logger->records);
        logger->records = NULL;
    }
}

/*==============================================================
 * 记录数据
 *==============================================================*/
void logger_record(WeightLogger* logger, RBFNetwork* net,
    double time_ms, double pos, double vel,
    double error, double ff_output)
{
    int i;
    WeightRecord* rec;

    logger->sample_counter++;
    if (logger->sample_counter < logger->sample_interval) return;
    logger->sample_counter = 0;
    if (logger->count >= logger->capacity) return;

    rec = &logger->records[logger->count];
    rec->time_ms = time_ms;
    rec->position = pos;
    rec->velocity = vel;
    rec->tracking_error = error;
    rec->ff_output = ff_output;
    for (i = 0; i < RBF_TOTAL_NODES; i++) {
        rec->weights[i] = net->weights[i];
    }
    logger->count++;
}

/*==============================================================
 * 保存完整记录
 *==============================================================*/
void logger_save_to_file(WeightLogger* logger, const char* filename)
{
    int i, j;
    FILE* fp = fopen(filename, "w");
    if (fp == NULL) return;

    fprintf(fp, "time_ms,position,velocity,tracking_error,ff_output");
    for (i = 0; i < RBF_TOTAL_NODES; i++) fprintf(fp, ",w%d", i);
    fprintf(fp, "\n");

    for (i = 0; i < logger->count; i++) {
        WeightRecord* rec = &logger->records[i];
        fprintf(fp, "%.2f,%.4f,%.4f,%.6f,%.6f",
            rec->time_ms, rec->position, rec->velocity,
            rec->tracking_error, rec->ff_output);
        for (j = 0; j < RBF_TOTAL_NODES; j++) {
            fprintf(fp, ",%.6f", rec->weights[j]);
        }
        fprintf(fp, "\n");
    }
    fclose(fp);
}

/*==============================================================
 * 保存摘要
 *==============================================================*/
void logger_save_summary(WeightLogger* logger, const char* filename)
{
    int i, j;
    FILE* fp = fopen(filename, "w");
    if (fp == NULL) return;

    fprintf(fp, "time_ms,tracking_error,ff_output,weight_norm\n");
    for (i = 0; i < logger->count; i++) {
        WeightRecord* rec = &logger->records[i];
        double norm = 0.0;
        for (j = 0; j < RBF_TOTAL_NODES; j++) {
            norm += rec->weights[j] * rec->weights[j];
        }
        fprintf(fp, "%.2f,%.6f,%.6f,%.6f\n",
            rec->time_ms, rec->tracking_error, rec->ff_output, sqrt(norm));
    }
    fclose(fp);
}

/*==============================================================
 * 预计算轨迹
 *==============================================================*/
int precomputeTrajectory()
{
    int i;
    double omega = 2.0 * PI / (double)SINE_PERIOD;

    g_desiredPos = (double*)malloc((TOTAL_SEG + 1) * sizeof(double));
    g_desiredVel = (double*)malloc((TOTAL_SEG + 1) * sizeof(double));
    if (g_desiredPos == NULL || g_desiredVel == NULL) return -1;

    g_desiredPos[0] = 0.0;
    for (i = 1; i <= TOTAL_SEG; i++) {
        double t = (double)(i * TIME_STEP);
        g_desiredPos[i] = SINE_AMPLITUDE * sin(omega * t);
    }

    g_desiredVel[0] = SINE_AMPLITUDE * omega;
    for (i = 1; i <= TOTAL_SEG; i++) {
        g_desiredVel[i] = (g_desiredPos[i] - g_desiredPos[i - 1]) / (double)TIME_STEP;
    }
    return 0;
}

/*==============================================================
 * 释放轨迹内存
 *==============================================================*/
void freeTrajectory()
{
    if (g_desiredPos) { free(g_desiredPos); g_desiredPos = NULL; }
    if (g_desiredVel) { free(g_desiredVel); g_desiredVel = NULL; }
}

/*==============================================================
 * 获取期望状态
 *==============================================================*/
void getDesiredState(double time_ms, double* pos, double* vel)
{
    int idx = (int)(time_ms / TIME_STEP);
    if (idx < 0) idx = 0;
    if (idx > TOTAL_SEG) idx = TOTAL_SEG;
    *pos = g_desiredPos[idx];
    *vel = g_desiredVel[idx];
}

/*==============================================================
 * 主函数
 *==============================================================*/
int main()
{
    short ret;
    long Status;
    long totalTime = TOTAL_SEG * TIME_STEP;
    int started = 0;
    int filled = 0;
    short space = 0;
    long mask = 1L << (AXIS - 1);
    long prfSts = 0;
    int i, j;

    double axisError = 0.0;
    double desiredPos = 0.0;
    double desiredVel = 0.0;
    double ffOutput = 0.0;
    double ffClamped = 0.0;
    double elapsed = 0.0;
    int updateCounter = 0;
    int learningEnabled = 1;

    /* ===== 新增：抗噪声滤波参数 ===== */
    double filtered_error = 0.0;
    double filtered_ff = 0.0;
    double effective_error = 0.0;

    // 滤波系数 (0.01~1.0)，越小越平滑，但也越滞后
    const double ALPHA_ERR = 0.05;
    const double ALPHA_FF = 0.1;

    // 误差死区 (脉冲数)，编码器在这个范围内跳动时，不让网络去瞎学习
    const double ERROR_DEADZONE = 5.0;
    /* ================================ */


    printf("============================================\n");
    printf("  RBF在线确定学习 - 速度环前馈补偿\n");
    printf("============================================\n");
    printf("正弦轨迹:\n");
    printf("  幅值: %.0f pulse\n", SINE_AMPLITUDE);
    printf("  周期: %d ms\n", SINE_PERIOD);
    printf("  周期数: %d\n", SINE_CYCLES);
    printf("  总段数: %d\n", TOTAL_SEG);
    printf("  总时间: %ld ms\n", totalTime);
    printf("\nRBF网络:\n");
    printf("  节点: %d x %d = %d\n", RBF_NODES_POS, RBF_NODES_VEL, RBF_TOTAL_NODES);
    printf("  学习率: %.4f\n", LEARNING_RATE);
    printf("\n安全限制:\n");
    printf("  前馈限幅: [%.1f, %.1f]\n", FF_OUTPUT_MIN, FF_OUTPUT_MAX);
    printf("  误差阈值: %.0f\n", ERROR_THRESHOLD);
    printf("============================================\n\n");

    /* 预计算轨迹 */
    printf("预计算期望轨迹...\n");
    if (precomputeTrajectory() != 0) {
        printf("轨迹内存分配失败!\n");
        return -1;
    }
    printf("  完成 (%d 个点)\n", TOTAL_SEG + 1);

    /* 初始化RBF */
    printf("初始化RBF网络...\n");
    rbf_init(&g_rbfNet);

    /* 初始化记录器 */
    printf("初始化记录器...\n");
    if (logger_init(&g_logger, LOG_CAPACITY, LOG_SAMPLE_INTERVAL) != 0) {
        printf("记录器初始化失败!\n");
        freeTrajectory();
        return -1;
    }

    /* 初始化控制卡 */
    QueryPerformanceFrequency(&g_freq);

    ret = GTN_OpenCard();
    if (ret != 0) {
        printf("GTN_OpenCard 失败, ret=%d\n", ret);
        logger_free(&g_logger);
        freeTrajectory();
        return -1;
    }
    printf("控制卡打开成功\n");

    ret = GTN_NetInit(
        NET_INIT_MODE_XML_STRICT,
        "C:\\Users\\MCLab\\Desktop\\MotionStudio_x64_V2.3.14.0_develop\\Googol.xml",
        60,
        &Status
    );
    if (ret != 0) {
        printf("GTN_NetInit 失败, ret=%d\n", ret);
        GTN_Close();
        logger_free(&g_logger);
        freeTrajectory();
        return -1;
    }
    printf("网络初始化成功\n");

    ret = GTN_ClrSts(CORE, AXIS, 1);
    printf("清状态 ret=%d\n", ret);

    ret = GTN_ZeroPos(CORE, AXIS, 1);
    printf("清零位置 ret=%d\n", ret);

    ret = GTN_AxisOn(CORE, AXIS);
    if (ret != 0) {
        printf("GTN_AxisOn 失败, ret=%d\n", ret);
        GTN_Close();
        logger_free(&g_logger);
        freeTrajectory();
        return -1;
    }
    Sleep(200);
    printf("轴已使能\n");

    /* 设置PT模式 */
    ret = GTN_PrfPt(CORE, AXIS, PT_MODE_DYNAMIC);
    if (ret != 0) {
        printf("GTN_PrfPt 失败, ret=%d\n", ret);
        goto EXIT;
    }
    printf("PT 动态模式设置成功\n");

    ret = GTN_SetPtMemory(CORE, AXIS, 0);
    if (ret != 0) {
        printf("GTN_SetPtMemory 失败, ret=%d\n", ret);
        goto EXIT;
    }

    GTN_PtClear(CORE, AXIS, 0);
    GTN_PtClear(CORE, AXIS, 1);
    printf("FIFO清空\n");

    printf("\n开始填充PT数据并运行RBF学习...\n");

    /* 主循环 */
    for (i = 1; i <= TOTAL_SEG; i++)
    {
        double t = (double)(i * TIME_STEP);
        double pos = SINE_AMPLITUDE * sin(2.0 * PI * t / (double)SINE_PERIOD);
        long endTime = i * TIME_STEP;
        short segType = (i == TOTAL_SEG) ? PT_SEG_STOP : PT_SEG_NORMAL;

        while (1)
        {
            ret = GTN_PtSpace(CORE, AXIS, &space, 0);
            if (ret != 0) {
                printf("GTN_PtSpace 失败, ret=%d\n", ret);
                goto EXIT;
            }

            if (space > 0) break;

            /* 启动PT */
            if (!started && filled >= FIFO0_SIZE) {
                GTN_PtStart(CORE, mask, 0);
                QueryPerformanceCounter(&g_startTime);
                started = 1;
                printf("PT已启动, 开始RBF学习\n");
            }

            /* RBF学习 (数据填充阶段) */
            if (started && learningEnabled) {

                elapsed = getElapsedMs();

                /* 启动阶段延迟补偿 */
                if (elapsed < 1000) {
                    //GTN_SetFrictionCompensateValue(CORE, AXIS, 0.0, 0.0);
                    Sleep(1);
                    continue;
                }

                GTN_GetAxisError(CORE, AXIS, &axisError, 1, NULL);
                getDesiredState(elapsed, &desiredPos, &desiredVel);

                /* 安全检查 */
                if (fabs(axisError) > ERROR_THRESHOLD) {
                    printf("\n误差过大 (%.2f), 停止学习!\n", axisError);
                    learningEnabled = 0;
                    //GTN_SetFrictionCompensateValue(CORE, AXIS, 0.0, 0.0);
                    continue;
                }

                /* 1. 误差低通滤波 */
                filtered_error = ALPHA_ERR * axisError + (1.0 - ALPHA_ERR) * filtered_error;

                /* 2. 误差死区 */
                effective_error = filtered_error;
                if (fabs(effective_error) < ERROR_DEADZONE) {
                    effective_error = 0.0;
                }

                /* 3. RBF计算原始输出 */
                ffOutput = rbf_compute_output(&g_rbfNet, desiredPos, desiredVel);

                /* 4. 输出信号低通滤波与限幅 */
                filtered_ff = ALPHA_FF * ffOutput + (1.0 - ALPHA_FF) * filtered_ff;
                ffClamped = clamp_output(filtered_ff, FF_OUTPUT_MIN, FF_OUTPUT_MAX);

                /* 5. 应用前馈 (注意：如果你想让电机实际生效，把最后的 0.0 改成 ffClamped) */
                //GTN_SetFrictionCompensateValue(CORE, AXIS, 0.0, ffClamped);

                /* 6. 降低更新频率，每 10ms 更新一次权重 */
                updateCounter++;
                if (updateCounter >= 10) {
                    rbf_update_weights(&g_rbfNet, effective_error);
                    updateCounter = 0;
                }

                /* 记录数据 (这里记录了实际生效的滤波后补偿量) */
                logger_record(&g_logger, &g_rbfNet, elapsed,
                    desiredPos, desiredVel, axisError, ffClamped);
            }

            Sleep(1);
        }

        ret = GTN_PtData(CORE, AXIS, pos, endTime, segType);
        if (ret != 0) {
            printf("GTN_PtData 失败: i=%d, ret=%d\n", i, ret);
            goto EXIT;
        }

        if (i <= 5 || i % 1000 == 0) {
            printf("  seg=%d/%d\n", i, TOTAL_SEG);
        }

        filled++;
    }

    printf("PT数据填充完成, 共 %d 段\n", filled);

    if (!started && filled > 0) {
        GTN_PtStart(CORE, mask, 0);
        QueryPerformanceCounter(&g_startTime);
        started = 1;
        printf("PT启动完成\n");
    }

    /* 等待运动完成 */
    printf("\n等待PT运动完成...\n");

    while (1)
    {
        GTN_GetPrfSts(CORE, AXIS, &prfSts, 1, NULL);
        if ((prfSts & (1L << 10)) == 0) break;

        elapsed = getElapsedMs();

        if (learningEnabled) {
            GTN_GetAxisError(CORE, AXIS, &axisError, 1, NULL);
            getDesiredState(elapsed, &desiredPos, &desiredVel);

            if (fabs(axisError) > ERROR_THRESHOLD) {
                printf("\n误差过大, 停止学习!\n");
                learningEnabled = 0;
                //GTN_SetFrictionCompensateValue(CORE, AXIS, 0.0, 0.0);
            }
            else {
                /* 1. 误差低通滤波 */
                filtered_error = ALPHA_ERR * axisError + (1.0 - ALPHA_ERR) * filtered_error;

                /* 2. 误差死区 */
                effective_error = filtered_error;
                if (fabs(effective_error) < ERROR_DEADZONE) {
                    effective_error = 0.0;
                }

                /* 3. RBF计算原始输出 */
                ffOutput = rbf_compute_output(&g_rbfNet, desiredPos, desiredVel);

                /* 4. 输出滤波限幅 */
                filtered_ff = ALPHA_FF * ffOutput + (1.0 - ALPHA_FF) * filtered_ff;
                ffClamped = clamp_output(filtered_ff, FF_OUTPUT_MIN, FF_OUTPUT_MAX);

                /* 5. 应用前馈 */
                //GTN_SetFrictionCompensateValue(CORE, AXIS, 0.0, ffClamped);

                /* 6. 更新权重 (每 10ms) */
                updateCounter++;
                if (updateCounter >= 10) {
                    rbf_update_weights(&g_rbfNet, effective_error);
                    updateCounter = 0;
                }

                logger_record(&g_logger, &g_rbfNet, elapsed,
                    desiredPos, desiredVel, axisError, ffClamped);
            }
        }

        if ((int)elapsed % 500 == 0) {
            printf("\r  时间: %.1f ms, 误差: %.2f, FF: %.2f   ", elapsed, axisError, ffClamped);
        }

        Sleep(1);
    }

    printf("\n\nPT运动完成! 时间: %.2f ms\n", getElapsedMs());

    /* 保存结果 */
    printf("\n保存数据...\n");
    logger_save_to_file(&g_logger, "rbf_learning_full.csv");
    logger_save_summary(&g_logger, "rbf_learning_summary.csv");

    {
        FILE* fp = fopen("rbf_final_weights.csv", "w");
        if (fp) {
            fprintf(fp, "node,pos_center,vel_center,weight\n");
            for (j = 0; j < RBF_TOTAL_NODES; j++) {
                fprintf(fp, "%d,%.4f,%.4f,%.6f\n", j,
                    g_rbfNet.centers[j].pos_center,
                    g_rbfNet.centers[j].vel_center,
                    g_rbfNet.weights[j]);
            }
            fclose(fp);
        }
    }
    printf("  rbf_learning_full.csv\n");
    printf("  rbf_learning_summary.csv\n");
    printf("  rbf_final_weights.csv\n");

    /* 统计 */
    {
        double norm = 0.0;
        for (j = 0; j < RBF_TOTAL_NODES; j++) {
            norm += g_rbfNet.weights[j] * g_rbfNet.weights[j];
        }
        printf("\n记录: %d 条, 权重范数: %.4f\n", g_logger.count, sqrt(norm));
    }

EXIT:
    //GTN_SetFrictionCompensateValue(CORE, AXIS, 0.0, 0.0);
    printf("\n前馈补偿已清除\n");

    GTN_AxisOff(CORE, AXIS);
    GTN_Close();
    printf("控制卡已关闭\n");

    logger_free(&g_logger);
    freeTrajectory();

    return 0;
}
