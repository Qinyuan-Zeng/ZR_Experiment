#include <conio.h>
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <math.h>
#include <string>
#include <cctype>
#include <limits>
#include <climits>
#include "gxn.h"

#pragma comment(lib, "winmm.lib")

using std::string;

#define CORE 2
#define AXIS 1
#ifdef PT_MODE_DYNAMIC
#undef PT_MODE_DYNAMIC
#endif
#define PT_MODE_DYNAMIC 1
#define PT_SEG_NORMAL 0

static const double PI = 3.14159265358979323846;
static const int LOOP_DELAY_MS = 1;
static const int TIME_STEP_MS = 1;
static const int SINE_PERIOD_MS = 4000;
static const int FIFO_PRELOAD = 64;
static const int FEED_BATCH_MAX = 64;
static const int UI_REFRESH_MS = 100;
static const int CSV_LOG_INTERVAL_MS = 1;
static const int CSV_FLUSH_MS = 500;
static const int START_RUN_WAIT_MS = 1500;
static const int START_RETRY_MS = 100;
static const int NO_CONSUME_TIMEOUT_MS = 300;
static const double SINE_AMPLITUDE_DEFAULT = 6666.667;
static const double AMP_RAMP_RATE_PER_SEC = 3000.0;
static const bool CHIRP_ENABLE = true;
static const double CHIRP_F_MIN_HZ = 1.0;
static const double CHIRP_F_MAX_HZ = 120.0;
static const double CHIRP_SWEEP_SEC = 30.0;
static const double CHIRP_STAGE1_FMAX_HZ = 40.0;
static const double CHIRP_STAGE2_FMAX_HZ = 120.0;
static const double CHIRP_F_ABS_MAX_HZ = 120.0;
static const double CHIRP_NYQUIST_MARGIN = 0.45;
static const double CHIRP_SWEEP_MIN_SEC = 1.0;
static const double CHIRP_SWEEP_MAX_SEC = 600.0;
static const double CHIRP_FREQ_SLEW_HZ_PER_SEC = 40.0;
static const double MAX_SAFETY_POS = 500000.0;
static const double AMP_MIN = 0.0;
static const double AMP_MAX = 46666.667;
static const char* BUILD_TAG = "PT_SIMPLE_PANEL_V6_CHIRP_DOB";
static const char* CSV_LOG_FILE = "chirp_trace.csv";

static void hideCursor() {
    CONSOLE_CURSOR_INFO cursor_info = { 1, 0 };
    SetConsoleCursorInfo(GetStdHandle(STD_OUTPUT_HANDLE), &cursor_info);
}

static void setCursorPosition(int x, int y) {
    COORD coord;
    coord.X = (SHORT)x;
    coord.Y = (SHORT)y;
    SetConsoleCursorPosition(GetStdHandle(STD_OUTPUT_HANDLE), coord);
}

static double rampTowards(double current, double target, double max_step) {
    if (current < target) {
        current += max_step;
        if (current > target) current = target;
    }
    else if (current > target) {
        current -= max_step;
        if (current < target) current = target;
    }
    return current;
}

static long calcSweepSamples(double sweep_sec) {
    double samples = sweep_sec * 1000.0 / (double)TIME_STEP_MS;
    if (samples < 1.0) samples = 1.0;
    if (samples > (double)LONG_MAX) samples = (double)LONG_MAX;
    return (long)(samples + 0.5);
}

static string trimLower(const string& src) {
    size_t b = 0;
    while (b < src.size() && std::isspace((unsigned char)src[b])) b++;
    size_t e = src.size();
    while (e > b && std::isspace((unsigned char)src[e - 1])) e--;
    string out = src.substr(b, e - b);
    for (size_t i = 0; i < out.size(); ++i) out[i] = (char)std::tolower((unsigned char)out[i]);
    return out;
}

static void normalizeChirpRange(double* fmin_hz, double* fmax_hz, double fmax_cap_hz) {
    if (*fmin_hz < 0.0) *fmin_hz = 0.0;
    if (*fmin_hz > fmax_cap_hz) *fmin_hz = fmax_cap_hz;
    if (*fmax_hz > fmax_cap_hz) *fmax_hz = fmax_cap_hz;
    if (*fmax_hz <= *fmin_hz) *fmax_hz = *fmin_hz + 0.1;
    if (*fmax_hz > fmax_cap_hz) *fmax_hz = fmax_cap_hz;
    if (*fmin_hz >= *fmax_hz) *fmin_hz = (*fmax_hz > 0.1) ? (*fmax_hz - 0.1) : 0.0;
}

int main() {
    timeBeginPeriod(1);

    short sRtn = 0;
    long pSts = 0;
    double prfPos = 0.0, encPos = 0.0, prfVel = 0.0, encVel = 0.0;
    double prev_prf_pos = 0.0, prev_enc_pos = 0.0;
    double center_prf_pos = 0.0, center_enc_pos = 0.0;

    printf("system init...\n");
    printf("build: %s %s | %s\n", __DATE__, __TIME__, BUILD_TAG);

    sRtn = GTN_OpenCard();
    if (sRtn != 0) { printf("GTN_OpenCard failed: %d\n", sRtn); return -1; }

    sRtn = GTN_NetInit(
        NET_INIT_MODE_XML_STRICT,
        const_cast<char*>("C:\\Users\\MCLab\\Desktop\\MotionStudio_x64_V2.3.14.0_develop\\Googol.xml"),
        60,
        &pSts
    );
    if (sRtn != 0) { printf("GTN_NetInit failed: %d\n", sRtn); GTN_Close(); return -1; }

    GTN_ClrSts(CORE, AXIS, 1);
    GTN_ZeroPos(CORE, AXIS, 1);
    GTN_SetPrfPos(CORE, AXIS, 0.0);

    sRtn = GTN_AxisOn(CORE, AXIS);
    if (sRtn != 0) { printf("GTN_AxisOn failed: %d\n", sRtn); GTN_Close(); return -1; }
    Sleep(200);

    GTN_ZeroPos(CORE, AXIS, 1);
    GTN_SetPrfPos(CORE, AXIS, 0.0);
    GTN_GetPrfPos(CORE, AXIS, &center_prf_pos, 1, NULL);
    GTN_GetEncPos(CORE, AXIS, &center_enc_pos, 1, NULL);

    sRtn = GTN_PrfPt(CORE, AXIS, PT_MODE_DYNAMIC);
    if (sRtn != 0) { printf("GTN_PrfPt failed: %d\n", sRtn); GTN_AxisOff(CORE, AXIS); GTN_Close(); return -1; }

    sRtn = GTN_SetPtMemory(CORE, AXIS, 0);
    if (sRtn != 0) { printf("GTN_SetPtMemory failed: %d\n", sRtn); GTN_AxisOff(CORE, AXIS); GTN_Close(); return -1; }

    GTN_PtClear(CORE, AXIS, 0);
    GTN_PtClear(CORE, AXIS, 1);

    system("cls");
    hideCursor();
    FILE* csv_fp = NULL;
    errno_t csv_open_err = fopen_s(&csv_fp, CSV_LOG_FILE, "w");
    if (csv_open_err == 0 && csv_fp) {
        fprintf(csv_fp, "time_ms,chirp_req_hz,chirp_eff_hz,amp_cur,amp_tgt,pos_cmd_abs,prf_pos_abs,enc_pos_abs,prf_vel,enc_vel,prf_rel,enc_rel,axis_sts,prf_sts\n");
        fflush(csv_fp);
    }
    else {
        printf("warn: cannot open %s\n", CSV_LOG_FILE);
    }

    bool is_running = true;
    bool is_estop = false;
    bool pt_started = false;

    string status_msg = "running";
    string exit_reason = "normal exit";
    string input_buffer;

    double amp_target = SINE_AMPLITUDE_DEFAULT;
    double amp_current = SINE_AMPLITUDE_DEFAULT;
    double pt_zero_bias = 0.0;
    double pos_cmd_abs = 0.0;
    double chirp_phase_rad = 0.0;
    double chirp_freq_hz = CHIRP_F_MIN_HZ;
    double chirp_req_freq_hz = CHIRP_F_MIN_HZ;
    double chirp_fmin_active = CHIRP_F_MIN_HZ;
    double chirp_fmax_active = CHIRP_F_MAX_HZ;
    double chirp_sweep_active = CHIRP_SWEEP_SEC;
    double chirp_fmin_pending = CHIRP_F_MIN_HZ;
    double chirp_fmax_pending = CHIRP_F_MAX_HZ;
    double chirp_sweep_pending = CHIRP_SWEEP_SEC;
    bool chirp_pending_apply = false;
    long chirp_sample_in_cycle = 0;
    long chirp_cycle_samples = calcSweepSamples(CHIRP_SWEEP_SEC);
    const double sample_rate_hz = 1000.0 / (double)TIME_STEP_MS;
    const double chirp_fmax_effective_cap = sample_rate_hz * CHIRP_NYQUIST_MARGIN;
    long pt_filled = 0;
    long traj_index = 0;
    long pt_end_time_ms = 0;
    short last_pt_ret = 0;
    short last_pt_start_ret = 0;
    short space = 0;
    int fifo_capacity = FIFO_PRELOAD;
    long last_axis_sts = 0;
    long last_prf_sts = 0;

    ULONGLONG start_clock = GetTickCount64();
    ULONGLONG prev_sample_clock = start_clock;
    ULONGLONG last_ui_tick = start_clock;
    ULONGLONG last_consume_tick = start_clock;
    ULONGLONG last_csv_tick = start_clock;
    ULONGLONG last_csv_flush_tick = start_clock;
    short prev_space = -1;
    long mask = 1L << (AXIS - 1);

    prev_prf_pos = center_prf_pos;
    prev_enc_pos = center_enc_pos;
    // Keep PT command centered at 0 so command/feedback ranges stay around [-amp, +amp].
    pt_zero_bias = 0.0;

    sRtn = GTN_PtSpace(CORE, AXIS, &space, 0);
    if (sRtn == 0) {
        if ((int)space > 0) fifo_capacity = (int)space;
        prev_space = space;
    }

    int preload_target = FIFO_PRELOAD;
    if (preload_target > fifo_capacity) preload_target = fifo_capacity;
    while (pt_filled < preload_target) {
        sRtn = GTN_PtSpace(CORE, AXIS, &space, 0);
        if (sRtn != 0) {
            printf("GTN_PtSpace failed during preload: %d\n", sRtn);
            GTN_AxisOff(CORE, AXIS);
            GTN_Close();
            timeEndPeriod(1);
            return -1;
        }
        if (space <= 0) {
            Sleep(1);
            continue;
        }

        long next_index = traj_index + 1;
        double s = 0.0;
        if (CHIRP_ENABLE) {
            if (chirp_sample_in_cycle == 0 && chirp_pending_apply) {
                chirp_fmin_active = chirp_fmin_pending;
                chirp_fmax_active = chirp_fmax_pending;
                chirp_sweep_active = chirp_sweep_pending;
                normalizeChirpRange(&chirp_fmin_active, &chirp_fmax_active,
                    (chirp_fmax_effective_cap < CHIRP_F_ABS_MAX_HZ) ? chirp_fmax_effective_cap : CHIRP_F_ABS_MAX_HZ);
                chirp_cycle_samples = calcSweepSamples(chirp_sweep_active);
                chirp_pending_apply = false;
            }
            double progress = (double)chirp_sample_in_cycle / (double)chirp_cycle_samples;
            chirp_req_freq_hz = chirp_fmin_active + (chirp_fmax_active - chirp_fmin_active) * progress;
            if (chirp_req_freq_hz > chirp_fmax_effective_cap) chirp_req_freq_hz = chirp_fmax_effective_cap;
            {
                double f_step = CHIRP_FREQ_SLEW_HZ_PER_SEC * ((double)TIME_STEP_MS / 1000.0);
                chirp_freq_hz = rampTowards(chirp_freq_hz, chirp_req_freq_hz, f_step);
            }
            chirp_phase_rad += 2.0 * PI * chirp_freq_hz * ((double)TIME_STEP_MS / 1000.0);
            if (chirp_phase_rad > 2.0 * PI) chirp_phase_rad = fmod(chirp_phase_rad, 2.0 * PI);
            s = sin(chirp_phase_rad);
            chirp_sample_in_cycle++;
            if (chirp_sample_in_cycle >= chirp_cycle_samples) chirp_sample_in_cycle = 0;
        }
        else {
            double phase_ms = (double)((next_index * TIME_STEP_MS) % SINE_PERIOD_MS);
            s = sin(2.0 * PI * phase_ms / (double)SINE_PERIOD_MS);
        }
        double amp_step = AMP_RAMP_RATE_PER_SEC * ((double)TIME_STEP_MS / 1000.0);
        amp_current = rampTowards(amp_current, amp_target, amp_step);
        pos_cmd_abs = pt_zero_bias + amp_current * s;

        long end_time_ms = (pt_filled + 1) * TIME_STEP_MS;
        last_pt_ret = GTN_PtData(CORE, AXIS, pos_cmd_abs, end_time_ms, PT_SEG_NORMAL);
        if (last_pt_ret != 0) {
            printf("GTN_PtData failed during preload: %d\n", last_pt_ret);
            GTN_AxisOff(CORE, AXIS);
            GTN_Close();
            timeEndPeriod(1);
            return -1;
        }
        traj_index = next_index;
        pt_filled++;
        pt_end_time_ms = end_time_ms;
    }

    last_pt_start_ret = GTN_PtStart(CORE, mask, 0);
    if (last_pt_start_ret != 0) {
        printf("GTN_PtStart failed: %d\n", last_pt_start_ret);
        GTN_AxisOff(CORE, AXIS);
        GTN_Close();
        timeEndPeriod(1);
        return -1;
    }
    pt_started = true;
    status_msg = "PT started";
    last_consume_tick = GetTickCount64();

    // Do not block here: FIFO would underflow and the axis would only move a few points then stop.
    GTN_GetPrfSts(CORE, AXIS, &last_prf_sts, 1, NULL);
    if ((last_prf_sts & (1L << 10)) != 0) {
        status_msg = "PT running";
    }
    else {
        status_msg = "PT started, streaming...";
    }

    while (is_running) {
        ULONGLONG now_tick = GetTickCount64();
        ULONGLONG sys_time_ms = now_tick - start_clock;
        double sys_t_s = sys_time_ms / 1000.0;

        if (_kbhit()) {
            char ch = (char)_getch();
            if ((unsigned char)ch == 0 || (unsigned char)ch == 224) {
                (void)_getch();
                ch = 0;
            }

            if (ch == 'q' || ch == 'Q') {
                is_estop = true;
                status_msg = "E-STOP by user";
                exit_reason = "E-STOP by user";
                GTN_Stop(CORE, mask, mask);
                GTN_PtClear(CORE, AXIS, 0);
                GTN_PtClear(CORE, AXIS, 1);
                break;
            }
            if (ch == 27) {// ESC
                is_running = false;
                status_msg = "stop requested";
                exit_reason = "stop requested by user";
                break;
            }
            if (ch == 13) {// Enter
                if (!input_buffer.empty()) {
                    try {
                        string cmd = trimLower(input_buffer);
                        bool handled = false;

                        if (cmd == "stage1") {
                            chirp_fmin_pending = CHIRP_F_MIN_HZ;
                            chirp_fmax_pending = CHIRP_STAGE1_FMAX_HZ;
                            chirp_sweep_pending = chirp_sweep_active;
                            normalizeChirpRange(&chirp_fmin_pending, &chirp_fmax_pending,
                                (chirp_fmax_effective_cap < CHIRP_F_ABS_MAX_HZ) ? chirp_fmax_effective_cap : CHIRP_F_ABS_MAX_HZ);
                            chirp_pending_apply = true;
                            status_msg = "chirp stage1 pending";
                            handled = true;
                        }
                        else if (cmd == "stage2") {
                            chirp_fmin_pending = CHIRP_F_MIN_HZ;
                            chirp_fmax_pending = CHIRP_STAGE2_FMAX_HZ;
                            chirp_sweep_pending = chirp_sweep_active;
                            normalizeChirpRange(&chirp_fmin_pending, &chirp_fmax_pending,
                                (chirp_fmax_effective_cap < CHIRP_F_ABS_MAX_HZ) ? chirp_fmax_effective_cap : CHIRP_F_ABS_MAX_HZ);
                            chirp_pending_apply = true;
                            status_msg = "chirp stage2 pending";
                            handled = true;
                        }
                        else if (cmd.rfind("fmin=", 0) == 0) {
                            double v = std::stod(cmd.substr(5));
                            if (v >= 0.0 && v <= CHIRP_F_ABS_MAX_HZ) {
                                chirp_fmin_pending = v;
                                normalizeChirpRange(&chirp_fmin_pending, &chirp_fmax_pending,
                                    (chirp_fmax_effective_cap < CHIRP_F_ABS_MAX_HZ) ? chirp_fmax_effective_cap : CHIRP_F_ABS_MAX_HZ);
                                chirp_pending_apply = true;
                                status_msg = "chirp fmin pending";
                            }
                            else status_msg = "bad fmin";
                            handled = true;
                        }
                        else if (cmd.rfind("fmax=", 0) == 0) {
                            double v = std::stod(cmd.substr(5));
                            if (v >= 0.0 && v <= CHIRP_F_ABS_MAX_HZ) {
                                chirp_fmax_pending = v;
                                normalizeChirpRange(&chirp_fmin_pending, &chirp_fmax_pending,
                                    (chirp_fmax_effective_cap < CHIRP_F_ABS_MAX_HZ) ? chirp_fmax_effective_cap : CHIRP_F_ABS_MAX_HZ);
                                chirp_pending_apply = true;
                                status_msg = "chirp fmax pending";
                            }
                            else status_msg = "bad fmax";
                            handled = true;
                        }
                        else if (cmd.rfind("sweep=", 0) == 0) {
                            double v = std::stod(cmd.substr(6));
                            if (v >= CHIRP_SWEEP_MIN_SEC && v <= CHIRP_SWEEP_MAX_SEC) {
                                chirp_sweep_pending = v;
                                chirp_pending_apply = true;
                                status_msg = "chirp sweep pending";
                            }
                            else status_msg = "bad sweep";
                            handled = true;
                        }
                        if (!handled) {
                            double new_amp = std::stod(cmd);
                            if (new_amp >= AMP_MIN && new_amp <= AMP_MAX) {
                                amp_target = new_amp;
                                status_msg = "amplitude target updated";
                            }
                            else {
                                status_msg = "amp out of range";
                            }
                        }
                    }
                    catch (...) {
                        status_msg = "invalid input";
                    }
                    input_buffer.clear();
                }
            }
            else if (ch == 8) {
                if (!input_buffer.empty()) input_buffer.pop_back();
            }
            else if (std::isprint((unsigned char)ch)) {
                input_buffer.push_back(ch);
            }
        }

        sRtn = GTN_PtSpace(CORE, AXIS, &space, 0);
        if (sRtn != 0) {
            last_pt_ret = sRtn;
            status_msg = "GTN_PtSpace failed";
            is_estop = true;
            exit_reason = "GTN_PtSpace failed";
            break;
        }

        int to_feed = (int)space;
        if (to_feed > FEED_BATCH_MAX) to_feed = FEED_BATCH_MAX;
        for (int i = 0; i < to_feed; ++i) {
            long next_index = traj_index + 1;
            double s = 0.0;
            if (CHIRP_ENABLE) {
                if (chirp_sample_in_cycle == 0 && chirp_pending_apply) {
                    chirp_fmin_active = chirp_fmin_pending;
                    chirp_fmax_active = chirp_fmax_pending;
                    chirp_sweep_active = chirp_sweep_pending;
                    normalizeChirpRange(&chirp_fmin_active, &chirp_fmax_active,
                        (chirp_fmax_effective_cap < CHIRP_F_ABS_MAX_HZ) ? chirp_fmax_effective_cap : CHIRP_F_ABS_MAX_HZ);
                    chirp_cycle_samples = calcSweepSamples(chirp_sweep_active);
                    chirp_pending_apply = false;
                }
                double progress = (double)chirp_sample_in_cycle / (double)chirp_cycle_samples;
                chirp_req_freq_hz = chirp_fmin_active + (chirp_fmax_active - chirp_fmin_active) * progress;
                if (chirp_req_freq_hz > chirp_fmax_effective_cap) chirp_req_freq_hz = chirp_fmax_effective_cap;
                {
                    double f_step = CHIRP_FREQ_SLEW_HZ_PER_SEC * ((double)TIME_STEP_MS / 1000.0);
                    chirp_freq_hz = rampTowards(chirp_freq_hz, chirp_req_freq_hz, f_step);
                }
                chirp_phase_rad += 2.0 * PI * chirp_freq_hz * ((double)TIME_STEP_MS / 1000.0);
                if (chirp_phase_rad > 2.0 * PI) chirp_phase_rad = fmod(chirp_phase_rad, 2.0 * PI);
                s = sin(chirp_phase_rad);
                chirp_sample_in_cycle++;
                if (chirp_sample_in_cycle >= chirp_cycle_samples) chirp_sample_in_cycle = 0;
            }
            else {
                double phase_ms = (double)((next_index * TIME_STEP_MS) % SINE_PERIOD_MS);
                s = sin(2.0 * PI * phase_ms / (double)SINE_PERIOD_MS);
            }

            double amp_step = AMP_RAMP_RATE_PER_SEC * ((double)TIME_STEP_MS / 1000.0);
            amp_current = rampTowards(amp_current, amp_target, amp_step);
            pos_cmd_abs = pt_zero_bias + amp_current * s;
            long end_time_ms = (pt_filled + 1) * TIME_STEP_MS;
            last_pt_ret = GTN_PtData(CORE, AXIS, pos_cmd_abs, end_time_ms, PT_SEG_NORMAL);
            if (last_pt_ret != 0) {
                status_msg = "PtData err = " + std::to_string(last_pt_ret);
                is_estop = true;
                exit_reason = "PtData failed";
                break;
            }
            traj_index = next_index;
            pt_filled++;
            pt_end_time_ms = end_time_ms;
        }
        if (is_estop) break;

        GTN_GetSts(CORE, AXIS, &last_axis_sts, 1, NULL);
        GTN_GetPrfPos(CORE, AXIS, &prfPos, 1, NULL);
        GTN_GetEncPos(CORE, AXIS, &encPos, 1, NULL);
        GTN_GetPrfSts(CORE, AXIS, &last_prf_sts, 1, NULL);

        if (prev_space >= 0 && space > prev_space) {
            last_consume_tick = now_tick;
        }
        else if ((now_tick - last_consume_tick) > NO_CONSUME_TIMEOUT_MS) {
            if ((last_prf_sts & (1L << 10)) == 0) {
                last_pt_start_ret = GTN_PtStart(CORE, mask, 0);
                status_msg = "re-issue PtStart";
                last_consume_tick = now_tick;
            }
        }
        prev_space = space;

        ULONGLONG dt_ms = now_tick - prev_sample_clock;
        if (dt_ms == 0) dt_ms = 1;
        prfVel = (prfPos - prev_prf_pos) / (double)dt_ms;
        encVel = (encPos - prev_enc_pos) / (double)dt_ms;
        prev_prf_pos = prfPos;
        prev_enc_pos = encPos;
        prev_sample_clock = now_tick;

        if ((last_axis_sts & (1 << 1)) || (last_axis_sts & (1 << 5)) || (last_axis_sts & (1 << 6)) || fabs(encPos) > MAX_SAFETY_POS) {
            is_estop = true;
            status_msg = "axis fault or over travel";
            exit_reason = "axis fault or over travel";
            break;
        }

        if (csv_fp && (now_tick - last_csv_tick) >= CSV_LOG_INTERVAL_MS) {
            fprintf(csv_fp, "%.3f,%.6f,%.6f,%.3f,%.3f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%ld,%ld\n",
                (double)sys_time_ms,
                chirp_req_freq_hz,
                chirp_freq_hz,
                amp_current,
                amp_target,
                pos_cmd_abs,
                prfPos,
                encPos,
                prfVel,
                encVel,
                prfPos - center_prf_pos,
                encPos - center_enc_pos,
                last_axis_sts,
                last_prf_sts);
            last_csv_tick = now_tick;
        }
        if (csv_fp && (now_tick - last_csv_flush_tick) >= CSV_FLUSH_MS) {
            fflush(csv_fp);
            last_csv_flush_tick = now_tick;
        }

        if ((now_tick - last_ui_tick) >= UI_REFRESH_MS) {
            setCursorPosition(0, 0);
            printf("==================== PT sine (simple panel) ====================\n");
            printf("time: %7.3f s   status: %-24s\n", sys_t_s, status_msg.c_str());
            printf("display basis: ABS (same baseline as scope)\n");
            printf("amp(cur): %10.2f   amp(tgt): %10.2f   ramp: %8.1f/s\n", amp_current, amp_target, AMP_RAMP_RATE_PER_SEC);
            printf("f(req/eff): %8.2f / %8.2f Hz   f[min,max]: [%6.2f, %6.2f]\n",
                chirp_req_freq_hz, chirp_freq_hz, chirp_fmin_active, chirp_fmax_active);
            printf("sweep: %6.2f s   fs: %6.1f Hz   effCap: %6.1f Hz\n",
                chirp_sweep_active, sample_rate_hz, chirp_fmax_effective_cap);
            printf("pending: %d  p[min,max,s]: [%.2f, %.2f, %.2f]   fifoCap: %4d\n",
                chirp_pending_apply ? 1 : 0, chirp_fmin_pending, chirp_fmax_pending, chirp_sweep_pending, fifo_capacity);
            printf("zeroBias(fixed0): %10.2f   centerPrf: %10.2f   centerEnc: %10.2f\n", pt_zero_bias, center_prf_pos, center_enc_pos);
            printf("pos cmd(abs): %10.2f   prfPos(abs): %10.2f   encPos(abs): %10.2f\n", pos_cmd_abs, prfPos, encPos);
            printf("debug rel:    %10.2f   prfRel:     %10.2f   encRel:     %10.2f\n",
                pos_cmd_abs - center_prf_pos, prfPos - center_prf_pos, encPos - center_enc_pos);
            printf("prfVel:  %10.4f   encVel: %10.4f\n", prfVel, encVel);
            printf("ptEnd:   %10ld ms   filled: %10ld   space: %5d\n", pt_end_time_ms, pt_filled, (int)space);
            printf("PtData:  %10d      PtStart:%10d   started:%5d\n", (int)last_pt_ret, (int)last_pt_start_ret, pt_started ? 1 : 0);
            printf("ahead:   %10ld ms   runBit10:%8d\n", pt_end_time_ms - (long)sys_time_ms, ((last_prf_sts & (1L << 10)) != 0) ? 1 : 0);
            printf("-----------------------------------------------------------------\n");
            printf("Enter amp [%.0f, %.0f] or cmd: stage1/stage2/fmin=x/fmax=x/sweep=x\n", AMP_MIN, AMP_MAX);
            printf("typing: %-48s_\n", input_buffer.c_str());
            printf("ESC: stop  |  Q: emergency stop\n");
            last_ui_tick = now_tick;
        }

        Sleep(LOOP_DELAY_MS);
    }

    setCursorPosition(0, 14);
    printf("\nprogram ending... reason: %s\n", exit_reason.c_str());

    if (is_estop) {
        GTN_Stop(CORE, mask, mask);
    }
    else {
        GTN_Stop(CORE, mask, 0);
    }

    Sleep(200);
    GTN_AxisOff(CORE, AXIS);
    GTN_Close();
    if (csv_fp) {
        fflush(csv_fp);
        fclose(csv_fp);
        csv_fp = NULL;
    }
    timeEndPeriod(1);
    printf("press any key to exit...\n");
    _getch();
    return 0;
}
