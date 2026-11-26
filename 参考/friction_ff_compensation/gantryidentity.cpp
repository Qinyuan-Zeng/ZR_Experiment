#include <conio.h>
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <math.h>     
#include <iostream>
#include <string>
#include <cctype>
#include "gxn.h"

#pragma comment(lib, "winmm.lib") 

using namespace std;

// ================= 宏定义 =================
#define AXIS 1
#define CORE 2

#define LOOP_DELAY_MS 1
const double PI = 3.14159265358979323846;

// ================= 辅助函数 =================
void commandhandler(const char* command, short error) {
    if (error != 0) printf("\n[ERROR] %s = %d\n", command, error);
}

void hideCursor() {
    CONSOLE_CURSOR_INFO cursor_info = { 1, 0 };
    SetConsoleCursorInfo(GetStdHandle(STD_OUTPUT_HANDLE), &cursor_info);
}

void setCursorPosition(int x, int y) {
    COORD coord;
    coord.X = x; coord.Y = y;
    SetConsoleCursorPosition(GetStdHandle(STD_OUTPUT_HANDLE), coord);
}

// ================= 主函数 =================
int main(int argc, char* argv[])
{
    // 提升 Windows 调度精度到 1ms
    timeBeginPeriod(1);

    short sRtn;
    long pSts;
    double prfPos, encPos, prfVel, encVel;

    printf("系统初始化中...\n");

    sRtn = GTN_OpenCard();
    commandhandler("GTN_OpenCard", sRtn);

    sRtn = GTN_NetInit(NET_INIT_MODE_XML_STRICT, const_cast<char*>("C:\\Users\\MCLab\\Desktop\\MotionStudio_x64_V2.3.14.0_develop\\Googol.xml"), 60, &pSts);
    if (sRtn != 0) { printf("网络初始化失败。\n"); return -1; }

    GTN_ClrSts(CORE, AXIS, 1);
    GTN_AxisOn(CORE, AXIS);
    Sleep(500);
    GTN_ZeroPos(CORE, AXIS);
    GTN_SetPrfPos(CORE, AXIS, 0);

    // 将AXIS轴设为PT动态模式并清空FIFO
    sRtn = GTN_PrfPt(CORE, AXIS, PT_MODE_DYNAMIC);
    commandhandler("GTN_PrfPt", sRtn);
    GTN_PtClear(CORE, AXIS);

    FILE* fp = nullptr;
#if defined(_MSC_VER)
    fopen_s(&fp, "pt_identification.csv", "w");
#else
    fp = fopen("pt_identification.csv", "w");
#endif
    if (!fp) return -1;
    fprintf(fp, "time_s,cmd_vel_ms,prf_vel_ms,enc_vel_ms,prf_pos,enc_pos\n");

    system("cls"); hideCursor();

    // ================= 关键交互与控制变量 =================
    bool is_running = true;
    bool is_estop = false;
    string error_msg = "正常运行中";

    double user_set_amp = 0.0;      // 目标正弦波速度的振幅 (pulse/ms)
    double SINE_FREQ_HZ = 0.5;      // 正弦波频率 0.5Hz (周期2秒)
    string input_buffer = "";

    const double MAX_SAFETY_POS = 500000.0;

    double pt_time_ms = 0.0;
    double prf_cmd_pos = 0.0;
    bool pt_started = false;

    long start_clock = timeGetTime();

    // ================= 实时控制与界面循环 =================
    while (is_running) {
        long sys_time_ms = timeGetTime() - start_clock;
        double sys_t_s = sys_time_ms / 1000.0;

        // --- 1. 读取按键交互 ---
        if (_kbhit()) {
            char ch = _getch();
            if (ch == 'q' || ch == 'Q') {
                is_estop = true; break;
            }
            else if (ch == ' ') {
                is_running = false;
                error_msg = ">>> 收到退出指令，正常断开并等待刹车... <<<";
                break;
            }
            else if (ch == 13) {
                if (!input_buffer.empty()) {
                    try {
                        double new_amp = stod(input_buffer);
                        if (new_amp >= 0.0 && new_amp <= 50.0) user_set_amp = new_amp;
                    }
                    catch (...) {}
                    input_buffer.clear();
                }
            }
            else if (ch == 8) {
                if (!input_buffer.empty()) input_buffer.pop_back();
            }
            else if (isdigit(ch) || ch == '.') {
                input_buffer += ch;
            }
        }

        // --- 2. 硬件状态安全校验 ---
        long axisSts = 0;
        short space = 0;
        GTN_GetSts(CORE, AXIS, &axisSts);
        GTN_PtSpace(CORE, AXIS, &space);

        GTN_GetPrfPos(CORE, AXIS, &prfPos);
        GTN_GetEncPos(CORE, AXIS, &encPos);
        GTN_GetPrfVel(CORE, AXIS, &prfVel);
        sRtn = GTN_GetAxisEncVel(CORE, AXIS, &encVel);
        if (sRtn != 0) encVel = 0.0;

        if ((axisSts & (1 << 1)) || (axisSts & (1 << 5)) || (axisSts & (1 << 6)) || fabs(encPos) > MAX_SAFETY_POS) {
            is_estop = true; break;
        }

        // --- 3. 浅水池算法: 生成 PT 底层正弦轨迹 ---
               // --- 3. 浅水池算法: 生成 PT 底层正弦轨迹 ---
        while (pt_time_ms < sys_time_ms + 15.0) {
            if (space <= 0) break; // 防止FIFO缓冲区堵死

            // 【关键修复】PT底层参数要求的 time，是当前运动跑了多长时间的“绝对累积时间”，我们需要把它递增起来！
            double next_time_ms = pt_time_ms + 1.0;
            double t_s = next_time_ms / 1000.0;

            // 计算这一个毫秒切片上的正弦波速度 v(t) = A * sin(2 * PI * f * t)
            double pt_vel = user_set_amp * sin(2.0 * PI * SINE_FREQ_HZ * t_s);

            // 对速度进行微元积分，得到当前累加的绝对指令位置 p
            prf_cmd_pos += pt_vel * 1.0;

            // 【关键修复】下发到底层！第四个参数传入累积产生的 next_time_ms，第五个选 PT_SEGMENT_NORMAL
            //short rtnData = GTN_PtData(CORE, AXIS, prf_cmd_pos, (long)next_time_ms, PT_SEGMENT_NORMAL);
            short rtnData = GTN_PtData(CORE, AXIS, prf_cmd_pos, 1, PT_SEGMENT_NORMAL);

            // 只有底层成功吃进去了这个点，我们的时间刻度才往后挪动 1 毫秒
            if (rtnData == 0) {
                pt_time_ms = next_time_ms;
            }
            else {
                break; // 系统拒绝接收，结束塞数据的动作
            }

            // 当缓存打入了 10ms 以上的连续路径后，触发一次性扳机开跑
            if (!pt_started && pt_time_ms >= 10.0) {
                GTN_PtStart(CORE, 1 << (AXIS - 1));
                pt_started = true;
            }
        }
        

        // --- 4. 写盘实时数据 ---
        // 算出当前的理想正弦指令速度记录在CSV里用于MATLAB对比
        double cur_ideal_cmd_vel = user_set_amp * sin(2.0 * PI * SINE_FREQ_HZ * sys_t_s);
        fprintf(fp, "%.4f,%.4f,%.4f,%.4f,%.0f,%.0f\n",
            sys_t_s, cur_ideal_cmd_vel, prfVel, encVel, prfPos, encPos);

        // --- 5. 界面刷新 ---
        if (sys_time_ms % 50 < 3) {
            setCursorPosition(0, 0);
            printf("====================== PT正弦波高频辨识面板 ======================\n");
            printf(" [时间] %6.2f s          | [系统通知] %-30s \n", sys_t_s, error_msg.c_str());
            printf("------------------------------------------------------------------\n");
            printf(" [指令] 正弦速度峰值(A)  = %5.2f pulse/ms \n", user_set_amp);
            printf(" [下发] 追踪积分位置(P)  = %8.0f 脉冲 (平滑正弦积分)\n", prf_cmd_pos);
            printf(" [反馈] 编码器实时速度   = %8.2f pulse/ms \n", encVel);
            printf(" [缓存] FIFO 提前量 = %4.0f ms\n", pt_time_ms - sys_time_ms);
            printf("------------------------------------------------------------------\n");
            printf("\n【安全操作规范：人肉探底法】\n");
            printf("  1. 请逐渐敲入数字并回车，提升正弦波的振幅。\n");
            printf("  待确认的输入值: %-10s_\n\n", input_buffer.c_str());
            printf("  [空格 Space] : 退出数据循环，自然停止\n");
            printf("  [ Q  键    ] : 触发【硬急停】，系统强制切断伺服抱死\n");
        }

        Sleep(LOOP_DELAY_MS);
    }

    // ================= 停机与清理 =================
    setCursorPosition(0, 18);
    printf("\n测试结束，执行停机序列...\n");

    if (fp) fclose(fp);

    if (is_estop) {
        GTN_Stop(CORE, 1 << (AXIS - 1), 1 << (AXIS - 1));
    }
    else {
        GTN_Stop(CORE, 1 << (AXIS - 1), 0);
    }

    Sleep(500);
    GTN_AxisOff(CORE, AXIS);
    GTN_Close();

    timeEndPeriod(1);

    printf("程序退出，数据已落盘。\n");
    return 0;
}