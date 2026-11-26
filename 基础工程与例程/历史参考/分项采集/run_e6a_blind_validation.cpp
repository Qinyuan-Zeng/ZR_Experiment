#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <conio.h>
#include <windows.h>
#include <mmsystem.h>
#include <algorithm>
#include <cmath>
#include <random>
#include <vector>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include "gxn.h"

#pragma comment(lib, "gxn.lib")
#pragma comment(lib, "winmm.lib")

#define CORE 2
#define AXIS 1

static const char* PACKAGE_VERSION = "modeling_e6a_blind_validation_v1";
static const char* DEFAULT_NET_XML_FILE = "Googol.xml";
static const char* DEFAULT_OUTPUT_ROOT = "E:\\A_ZQY\\Experiment\\ModelingExperiment";
static const char* CONDITION_ID = "e6a_blind_intermediate_combinations";
static const char* REFERENCE_CONDITION_ID = "e6a_reference_x85_v50_a1000";
static const char* WARMUP_CONDITION_ID = "e6a_warmup_x85_v50_a1000";
static const char* CONTACT_CONFIG = "no_contact_confirmed";
static const char* FROZEN_MODEL_VERSION = "semimech_baseline_model_v3";
static const char* FROZEN_MODEL_SHA256 = "48EB0EBB81274713659A4B2ADAC42C7A3BA8CAC0E28E21DD70D42E1797A6F550";

static const double PULSE_PER_MM = 10000.0;
static const double IFB_RATED_CURRENT_ARMS = 4.500;
static const double ATL_TORQUE_FULL_SCALE = 1000.0;
static const double CURRENT_SIGN = 1.0;

// The controller zero is captured at physical x=0 mm. The experiment itself
// starts at x=5 mm, reached by an unlogged initialization move.
static const double X_ZERO_MM = 0.0;
static const double X_START_MM = 5.0;
static const double X_TARGET_MM = 87.5;
static const double X_REFERENCE_TARGET_MM = 85.0;
static const double SOFT_LIMIT_LOW_MM = 0.0;
static const double SOFT_LIMIT_HIGH_MM = 100.0;
static const double GUARD_LOW_MM = 1.5;
static const double GUARD_HIGH_MM = 99.5;
static const double START_TOLERANCE_MM = 0.20;
static const double REFERENCE_VELOCITY_MM_S = 50.0;
static const double INITIAL_POSITION_VELOCITY_MM_S = 10.0;
static const double REFERENCE_ACCELERATION_MM_S2 = 1000.0;
static const double FORWARD_ACCELERATION_MM_S2 = 1000.0;
static const double PROFILE_ACCEL_FILTER_TAU_S = 0.010;
static const double ACTUAL_ACCEL_FILTER_TAU_S = 0.020;
static const short TRAP_SMOOTH_TIME_MS = 25;
static const int WARMUP_COUNT = 12;
static const int E6A_TRIAL_COUNT = 50;
static const int E0_REFERENCE_COUNT = 6;
static const int FORMAL_ITEM_COUNT = E6A_TRIAL_COUNT + E0_REFERENCE_COUNT;
static const int TOTAL_TRAJECTORY_COUNT = WARMUP_COUNT + FORMAL_ITEM_COUNT;
static const unsigned int RANDOM_SEED = 2026093003u;
static const double WARMUP_PERIOD_S = 30.0;
static const double FORMAL_PERIOD_S = 30.0;
static const double INITIAL_WAIT_S = 5.0;
static const double ENDPOINT_HOLD_S = 0.50;
static const double FORMAL_HOLD_S = 3.00;
static const double MOTION_TIMEOUT_EXTRA_S = 5.0;
static const DWORD IDLE_SAMPLE_SLEEP_MS = 5;

struct RunContext {
    LARGE_INTEGER freq;
    LARGE_INTEGER t0;
    LARGE_INTEGER prev_sample;
    bool have_prev_sample;
    double unix_t0_s;
    double zero_axis_enc_pulse;
    double zero_raw_enc_pulse;
    long long next_sample_index;
    long long row_count;
    bool safety_stop;
    bool api_failure;
    std::string session_id;
    int current_trial_id;
    std::string current_reference_id;
    std::string current_condition_id;
    const char* current_validation_role;
    const char* current_calibration_window;
    double current_target_mm;
    double current_hold_time_s;
    double current_velocity_mm_s;
    double current_acceleration_mm_s2;
    double current_stop_acceleration_mm_s2;
    double prev_prf_velocity_mm_s;
    double prev_actual_velocity_mm_s;
    double filtered_a_profile_mm_s2;
    double filtered_a_est_mm_s2;
    bool have_acceleration_state;
    FILE* csv;
};

static void commandHandler(const char* command, short error) {
    if (error != 0) std::printf("[ERROR] %s = %d\n", command, error);
}

static double qpcSeconds(const LARGE_INTEGER& c, const LARGE_INTEGER& f) {
    return static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart);
}

static double qpcSinceStart(const LARGE_INTEGER& c, const RunContext& ctx) {
    return qpcSeconds(c, ctx.freq) - qpcSeconds(ctx.t0, ctx.freq);
}

static double unixTimeNowSeconds() {
    FILETIME ft = {};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER v = {};
    v.LowPart = ft.dwLowDateTime;
    v.HighPart = ft.dwHighDateTime;
    return (static_cast<double>(v.QuadPart) - 116444736000000000.0) / 10000000.0;
}

static long mmToPulseLong(double mm) {
    return static_cast<long>((mm >= 0.0) ? (mm * PULSE_PER_MM + 0.5)
                                         : (mm * PULSE_PER_MM - 0.5));
}

static long physicalMmToPulse(double physical_mm) {
    return mmToPulseLong(physical_mm - X_ZERO_MM);
}

static double pulseToMm(double pulse) { return pulse / PULSE_PER_MM; }
static double pulseToPhysicalMm(double pulse) { return X_ZERO_MM + pulseToMm(pulse); }
static double maxDouble(double a, double b) { return (a > b) ? a : b; }
static double currentRawToArms(short raw) {
    return CURRENT_SIGN * static_cast<double>(raw) * IFB_RATED_CURRENT_ARMS / ATL_TORQUE_FULL_SCALE;
}

static bool ensureDirectory(const std::string& path) {
    DWORD attr = GetFileAttributesA(path.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) return true;
    return CreateDirectoryA(path.c_str(), NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

static bool ensurePackageDirs(const std::string& root) {
    return ensureDirectory(root) && ensureDirectory(root + "\\data") &&
           ensureDirectory(root + "\\data\\raw") && ensureDirectory(root + "\\manifest") &&
           ensureDirectory(root + "\\results");
}

static bool fileExists(const std::string& path) {
    DWORD attr = GetFileAttributesA(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static std::string makeSessionId() {
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    char buf[64] = {};
    sprintf_s(buf, sizeof(buf), "%04d%02d%02d_%02d%02d%02d",
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return std::string(buf);
}

static bool isAxisFault(long s) {
    return ((s & (1 << 1)) || (s & (1 << 5)) || (s & (1 << 6)));
}

static bool isMoving(long s) { return (s & 0x400) != 0; }

static bool userRequestedStop() {
    if (!_kbhit()) return false;
    int ch = _getch();
    return ch == 'q' || ch == 'Q';
}

static const char* directionForPhase(const char* phase) {
    if (std::strcmp(phase, "move_forward") == 0) return "positive";
    if (std::strcmp(phase, "move_return") == 0) return "negative";
    return "stationary";
}

static bool writeCsvHeader(FILE* fp) {
    if (!fp) return false;
    std::fprintf(fp,
        "timestamp_unix_s,time_s,run_time,session_id,trial_id,condition_id,phase,phase_time,reference_id,model_version,"
        "x_cmd_mm,x_act_mm,x_prf_mm,v_cmd_mm_s,v_act_mm_s,a_cmd_mm_s2,a_profile_mm_s2,a_est_mm_s2,jerk_cmd_mm_s3,current_raw,current_A,current_command,"
        "target_mm,v_max_mm_s,a_max_mm_s2,a_stop_mm_s2,hold_time_s,direction,contact_config,"
        "position_error_mm,velocity_error_mm_s,servo_state,temperature,validation_role,calibration_window,delta_b_trial,"
        "safety_stop,api_failure,sample_index,phase_sample_index,dt_s,api_duration_us,planner_status,"
        "limit_positive,limit_negative,enc_source,trap_smooth_time_ms\n");
    return std::ferror(fp) == 0;
}

static bool readAndWriteSample(RunContext& ctx, int ref, const char* phase,
                               long phase_index, double phase_start_s,
                               double x_cmd_mm, double v_cmd_mm_s) {
    if (!ctx.csv) return false;
    LARGE_INTEGER sample_start = {}, api_start = {}, api_end = {};
    QueryPerformanceCounter(&sample_start);
    QueryPerformanceCounter(&api_start);

    double dt_s = 0.0;
    if (ctx.have_prev_sample) dt_s = qpcSeconds(sample_start, ctx.freq) - qpcSeconds(ctx.prev_sample, ctx.freq);
    else ctx.have_prev_sample = true;
    ctx.prev_sample = sample_start;
    const double run_time_s = qpcSinceStart(sample_start, ctx);

    long axis_status = 0, planner_status = 0;
    double prf_pos = 0.0, prf_vel = 0.0, axis_enc = 0.0, raw_enc = 0.0;
    double axis_enc_vel = 0.0, axis_error = 0.0;
    short lim_pos = 0, lim_neg = 0, current_raw = 0;
    short r_sts = GTN_GetSts(CORE, AXIS, &axis_status, 1, NULL);
    short r_prf_sts = GTN_GetPrfSts(CORE, AXIS, &planner_status, 1, NULL);
    short r_prf_pos = GTN_GetPrfPos(CORE, AXIS, &prf_pos, 1, NULL);
    short r_prf_vel = GTN_GetPrfVel(CORE, AXIS, &prf_vel, 1, NULL);
    short r_axis_pos = GTN_GetAxisEncPos(CORE, AXIS, &axis_enc, 1, NULL);
    short r_raw_pos = GTN_GetEncPos(CORE, AXIS, &raw_enc, 1, NULL);
    short r_axis_vel = GTN_GetAxisEncVel(CORE, AXIS, &axis_enc_vel, 1, NULL);
    short r_error = GTN_GetAxisError(CORE, AXIS, &axis_error, 1, NULL);
    short r_limit = GTN_GetLimitStatus(CORE, AXIS, &lim_pos, &lim_neg);
    short r_current = GTN_GetAtlTorque(CORE, AXIS, &current_raw);
    QueryPerformanceCounter(&api_end);

    const bool api_failure = r_sts || r_prf_sts || r_prf_pos || r_prf_vel ||
        (r_axis_pos && r_raw_pos) || r_axis_vel || r_error || r_limit || r_current;
    if (api_failure) ctx.api_failure = true;

    double x_act = std::numeric_limits<double>::quiet_NaN();
    const char* enc_source = "none";
    if (r_axis_pos == 0) {
        x_act = X_ZERO_MM + pulseToMm(axis_enc - ctx.zero_axis_enc_pulse);
        enc_source = "axis";
    } else if (r_raw_pos == 0) {
        x_act = X_ZERO_MM + pulseToMm(raw_enc - ctx.zero_raw_enc_pulse);
        enc_source = "raw";
    }
    const double v_act = pulseToMm(axis_enc_vel * 1000.0);
    const double v_profile = pulseToMm(prf_vel * 1000.0);
    if (!ctx.have_acceleration_state || dt_s <= 0.0 || dt_s > 0.050) {
        ctx.prev_prf_velocity_mm_s = v_profile;
        ctx.prev_actual_velocity_mm_s = v_act;
        ctx.filtered_a_profile_mm_s2 = 0.0;
        ctx.filtered_a_est_mm_s2 = 0.0;
        ctx.have_acceleration_state = true;
    } else {
        const double a_profile_raw = (v_profile - ctx.prev_prf_velocity_mm_s) / dt_s;
        const double a_est_raw = (v_act - ctx.prev_actual_velocity_mm_s) / dt_s;
        const double alpha_profile = dt_s / (PROFILE_ACCEL_FILTER_TAU_S + dt_s);
        const double alpha_actual = dt_s / (ACTUAL_ACCEL_FILTER_TAU_S + dt_s);
        ctx.filtered_a_profile_mm_s2 += alpha_profile *
            (a_profile_raw - ctx.filtered_a_profile_mm_s2);
        ctx.filtered_a_est_mm_s2 += alpha_actual *
            (a_est_raw - ctx.filtered_a_est_mm_s2);
        ctx.prev_prf_velocity_mm_s = v_profile;
        ctx.prev_actual_velocity_mm_s = v_act;
    }
    const double nan = std::numeric_limits<double>::quiet_NaN();
    if (api_failure || isAxisFault(axis_status) || lim_pos || lim_neg || !std::isfinite(x_act) ||
        x_act < GUARD_LOW_MM || x_act > GUARD_HIGH_MM) ctx.safety_stop = true;

    const double api_us = (qpcSeconds(api_end, ctx.freq) - qpcSeconds(api_start, ctx.freq)) * 1e6;
    std::fprintf(ctx.csv,
        "%.9f,%.9f,%.9f,%s,%d,%s,%s,%.9f,%s,%s,"
        "%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%d,%.9f,%.9f,"
        "%.9f,%.9f,%.9f,%.9f,%.9f,%s,%s,"
        "%.9f,%.9f,0x%08lX,%.9f,%s,%s,%.9f,"
        "%d,%d,%lld,%ld,%.9f,%.3f,0x%08lX,%d,%d,%s,%d\n",
        ctx.unix_t0_s + run_time_s, run_time_s, run_time_s, ctx.session_id.c_str(), ctx.current_trial_id,
        ctx.current_condition_id.c_str(), phase, run_time_s - phase_start_s,
        ctx.current_reference_id.c_str(), PACKAGE_VERSION,
        x_cmd_mm, x_act, pulseToPhysicalMm(prf_pos), v_cmd_mm_s, v_act,
        ctx.current_acceleration_mm_s2, ctx.filtered_a_profile_mm_s2, ctx.filtered_a_est_mm_s2, nan, current_raw, currentRawToArms(current_raw), nan,
        ctx.current_target_mm, ctx.current_velocity_mm_s, ctx.current_acceleration_mm_s2, ctx.current_stop_acceleration_mm_s2,
        ctx.current_hold_time_s, directionForPhase(phase), CONTACT_CONFIG,
        pulseToMm(axis_error), v_cmd_mm_s - v_act, axis_status, nan,
        ctx.current_validation_role, ctx.current_calibration_window, nan,
        ctx.safety_stop ? 1 : 0, api_failure ? 1 : 0,
        ctx.next_sample_index++, phase_index, dt_s, api_us, planner_status,
        lim_pos, lim_neg, enc_source, TRAP_SMOOTH_TIME_MS);
    if (std::ferror(ctx.csv)) {
        ctx.api_failure = true;
        ctx.safety_stop = true;
    }
    ++ctx.row_count;
    return !ctx.safety_stop;
}

static bool sampleForDuration(RunContext& ctx, int ref, const char* phase,
                              double duration_s, double x_cmd, double v_cmd,
                              DWORD sleep_ms) {
    LARGE_INTEGER start = {};
    QueryPerformanceCounter(&start);
    const double phase_start_s = qpcSinceStart(start, ctx);
    long phase_index = 0;
    while (true) {
        if (userRequestedStop()) ctx.safety_stop = true;
        if (!readAndWriteSample(ctx, ref, phase, phase_index++, phase_start_s, x_cmd, v_cmd)) return false;
        LARGE_INTEGER now = {};
        QueryPerformanceCounter(&now);
        if (qpcSeconds(now, ctx.freq) - qpcSeconds(start, ctx.freq) >= duration_s) return true;
        if (sleep_ms > 0) Sleep(sleep_ms);
    }
}

static bool sampleUntilRunTime(RunContext& ctx, int ref, double target_s) {
    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);
    const double remaining = target_s - qpcSinceStart(now, ctx);
    if (remaining <= 0.0) return true;
    return sampleForDuration(ctx, ref, "wait_at_start", remaining, X_START_MM, 0.0,
                             IDLE_SAMPLE_SLEEP_MS);
}

static bool configureTrap(double acceleration_mm_s2, double deceleration_mm_s2) {
    if (acceleration_mm_s2 < 100.0 || acceleration_mm_s2 > 2500.0 ||
        deceleration_mm_s2 < 100.0 || deceleration_mm_s2 > 2500.0) return false;
    TTrapPrm trap = {};
    short ret = GTN_GetTrapPrm(CORE, AXIS, &trap);
    commandHandler("GTN_GetTrapPrm", ret);
    if (ret != 0) return false;
    trap.acc = acceleration_mm_s2 * PULSE_PER_MM / 1000000.0;
    trap.dec = deceleration_mm_s2 * PULSE_PER_MM / 1000000.0;
    trap.velStart = 0.0;
    trap.smoothTime = TRAP_SMOOTH_TIME_MS;
    ret = GTN_SetTrapPrm(CORE, AXIS, &trap);
    commandHandler("GTN_SetTrapPrm", ret);
    return ret == 0;
}

static bool moveToExperimentStart(RunContext& ctx) {
    std::printf("Initializing coordinate system: x=0 -> experiment start x=5 mm at %.1f mm/s.\n",
                INITIAL_POSITION_VELOCITY_MM_S);
    short ret = GTN_SetPos(CORE, AXIS, physicalMmToPulse(X_START_MM));
    commandHandler("GTN_SetPos(initial_to_start)", ret);
    if (ret != 0) return false;
    ret = GTN_SetVel(CORE, AXIS, INITIAL_POSITION_VELOCITY_MM_S * PULSE_PER_MM / 1000.0);
    commandHandler("GTN_SetVel(initial_to_start)", ret);
    if (ret != 0) return false;
    ret = GTN_Update(CORE, 1L << (AXIS - 1));
    commandHandler("GTN_Update(initial_to_start)", ret);
    if (ret != 0) return false;

    const ULONGLONG start_ms = GetTickCount64();
    while (true) {
        if (userRequestedStop()) {
            ctx.safety_stop = true;
            return false;
        }
        long axis_status = 0;
        short lim_pos = 0, lim_neg = 0;
        double axis_enc = 0.0, raw_enc = 0.0;
        const short r_sts = GTN_GetSts(CORE, AXIS, &axis_status, 1, NULL);
        const short r_lim = GTN_GetLimitStatus(CORE, AXIS, &lim_pos, &lim_neg);
        const short r_axis = GTN_GetAxisEncPos(CORE, AXIS, &axis_enc, 1, NULL);
        const short r_raw = GTN_GetEncPos(CORE, AXIS, &raw_enc, 1, NULL);
        if (r_sts != 0 || r_lim != 0 || (r_axis != 0 && r_raw != 0)) {
            ctx.api_failure = true;
            ctx.safety_stop = true;
            return false;
        }
        const double x_act = (r_axis == 0)
            ? pulseToPhysicalMm(axis_enc - ctx.zero_axis_enc_pulse)
            : pulseToPhysicalMm(raw_enc - ctx.zero_raw_enc_pulse);
        if (isAxisFault(axis_status) || lim_pos || lim_neg || !std::isfinite(x_act) ||
            x_act < X_ZERO_MM - START_TOLERANCE_MM || x_act > X_START_MM + 0.50) {
            std::printf("Initial 0-to-5 mm positioning safety check failed: x=%.4f mm.\n", x_act);
            ctx.safety_stop = true;
            return false;
        }
        if (!isMoving(axis_status)) {
            if (std::fabs(x_act - X_START_MM) > START_TOLERANCE_MM) {
                std::printf("Initial x=5 mm arrival check failed: x=%.4f mm.\n", x_act);
                ctx.safety_stop = true;
                return false;
            }
            std::printf("Experiment start reached: x=%.4f mm. Data logging will begin next.\n", x_act);
            return true;
        }
        if (GetTickCount64() - start_ms > 8000ULL) {
            std::printf("Initial 0-to-5 mm positioning timeout.\n");
            ctx.safety_stop = true;
            return false;
        }
        Sleep(1);
    }
}
static bool moveAndSample(RunContext& ctx, int ref, const char* phase,
                          double target_mm, double signed_v_mm_s) {
    if (target_mm < X_START_MM || target_mm > X_TARGET_MM) return false;
    short ret = GTN_SetPos(CORE, AXIS, physicalMmToPulse(target_mm));
    commandHandler("GTN_SetPos", ret);
    if (ret != 0) return false;
    ret = GTN_SetVel(CORE, AXIS, std::fabs(signed_v_mm_s) * PULSE_PER_MM / 1000.0);
    commandHandler("GTN_SetVel", ret);
    if (ret != 0) return false;
    ret = GTN_Update(CORE, 1L << (AXIS - 1));
    commandHandler("GTN_Update", ret);
    if (ret != 0) return false;

    const double travel = std::fabs(target_mm - X_START_MM);
    const double speed = maxDouble(1.0, std::fabs(signed_v_mm_s));
    const double timeout_s = maxDouble(8.0, travel / speed + speed / maxDouble(100.0, ctx.current_acceleration_mm_s2) + MOTION_TIMEOUT_EXTRA_S);
    LARGE_INTEGER start = {};
    QueryPerformanceCounter(&start);
    const double phase_start_s = qpcSinceStart(start, ctx);
    long phase_index = 0;
    while (true) {
        if (userRequestedStop()) ctx.safety_stop = true;
        if (!readAndWriteSample(ctx, ref, phase, phase_index++, phase_start_s,
                                target_mm, signed_v_mm_s)) return false;
        long axis_status = 0;
        if (GTN_GetSts(CORE, AXIS, &axis_status, 1, NULL) != 0) {
            ctx.api_failure = true;
            ctx.safety_stop = true;
            return false;
        }
        if (!isMoving(axis_status)) return true;
        LARGE_INTEGER now = {};
        QueryPerformanceCounter(&now);
        if (qpcSeconds(now, ctx.freq) - qpcSeconds(start, ctx.freq) > timeout_s) {
            std::printf("Motion timeout: reference=%d phase=%s\n", ref, phase);
            ctx.safety_stop = true;
            return false;
        }
    }
}

static void setItemMetadata(RunContext& ctx, const char* kind, int index,
                            double target_mm = X_REFERENCE_TARGET_MM,
                            double velocity_mm_s = REFERENCE_VELOCITY_MM_S,
                            double acceleration_mm_s2 = REFERENCE_ACCELERATION_MM_S2) {
    char id[112] = {};
    char cid[112] = {};
    ctx.current_target_mm = target_mm;
    ctx.current_velocity_mm_s = velocity_mm_s;
    ctx.current_acceleration_mm_s2 = acceleration_mm_s2;
    ctx.current_stop_acceleration_mm_s2 = acceleration_mm_s2;
    if (std::strcmp(kind, "warmup") == 0) {
        sprintf_s(id, sizeof(id), "warmup_%02d", index);
        ctx.current_trial_id = 0;
        ctx.current_condition_id = WARMUP_CONDITION_ID;
        ctx.current_validation_role = "warmup";
        ctx.current_calibration_window = "none";
        ctx.current_hold_time_s = FORMAL_HOLD_S;
        ctx.current_target_mm = X_REFERENCE_TARGET_MM;
        ctx.current_velocity_mm_s = REFERENCE_VELOCITY_MM_S;
        ctx.current_acceleration_mm_s2 = REFERENCE_ACCELERATION_MM_S2;
        ctx.current_stop_acceleration_mm_s2 = REFERENCE_ACCELERATION_MM_S2;
    } else if (std::strcmp(kind, "reference") == 0) {
        sprintf_s(id, sizeof(id), "ref_%02d", index);
        ctx.current_trial_id = 0;
        ctx.current_condition_id = REFERENCE_CONDITION_ID;
        ctx.current_validation_role = "session_reference";
        ctx.current_calibration_window = "drift_and_hold_session_reference";
        ctx.current_hold_time_s = FORMAL_HOLD_S;
        ctx.current_target_mm = X_REFERENCE_TARGET_MM;
        ctx.current_velocity_mm_s = REFERENCE_VELOCITY_MM_S;
        ctx.current_acceleration_mm_s2 = REFERENCE_ACCELERATION_MM_S2;
        ctx.current_stop_acceleration_mm_s2 = REFERENCE_ACCELERATION_MM_S2;
    } else {
        const int target_deci_mm = static_cast<int>(target_mm * 10.0 + 0.5);
        sprintf_s(id, sizeof(id), "e6a_trial_%02d_x%04d_v%03d_a%04d", index,
                  target_deci_mm, static_cast<int>(velocity_mm_s + 0.5),
                  static_cast<int>(acceleration_mm_s2 + 0.5));
        sprintf_s(cid, sizeof(cid), "e6a_blind_x%04d_v%03d_a%04d",
                  target_deci_mm, static_cast<int>(velocity_mm_s + 0.5),
                  static_cast<int>(acceleration_mm_s2 + 0.5));
        ctx.current_trial_id = index;
        ctx.current_condition_id = cid;
        ctx.current_validation_role = "blind_same_day";
        ctx.current_calibration_window = "free_move_adaptive_x10_to_min55_or_target_minus2";
        ctx.current_hold_time_s = FORMAL_HOLD_S;
    }
    ctx.current_reference_id = id;
}

static bool runReferenceTrajectory(RunContext& ctx, int ref) {
    if (!configureTrap(REFERENCE_ACCELERATION_MM_S2, REFERENCE_ACCELERATION_MM_S2)) return false;
    std::printf("%s %d: 5 -> 85 -> 5 mm at v=%.1f, a=%.0f mm/s^2\n",
                ctx.current_validation_role, ref, REFERENCE_VELOCITY_MM_S,
                REFERENCE_ACCELERATION_MM_S2);
    if (!moveAndSample(ctx, ref, "move_forward", X_REFERENCE_TARGET_MM,
                       REFERENCE_VELOCITY_MM_S)) return false;
    if (!sampleForDuration(ctx, ref, "hold_target", FORMAL_HOLD_S,
                           X_REFERENCE_TARGET_MM, 0.0, IDLE_SAMPLE_SLEEP_MS)) return false;
    ctx.current_target_mm = X_START_MM;
    ctx.current_velocity_mm_s = REFERENCE_VELOCITY_MM_S;
    ctx.current_acceleration_mm_s2 = REFERENCE_ACCELERATION_MM_S2;
    ctx.current_stop_acceleration_mm_s2 = REFERENCE_ACCELERATION_MM_S2;
    if (!configureTrap(REFERENCE_ACCELERATION_MM_S2, REFERENCE_ACCELERATION_MM_S2)) return false;
    if (!moveAndSample(ctx, ref, "move_return", X_START_MM,
                       -REFERENCE_VELOCITY_MM_S)) return false;
    return sampleForDuration(ctx, ref, "hold_start", ENDPOINT_HOLD_S,
                             X_START_MM, 0.0, IDLE_SAMPLE_SLEEP_MS);
}

static bool runBlindTrial(RunContext& ctx, int trial, double target_mm,
                          double velocity_mm_s, double acceleration_mm_s2) {
    std::printf("blind %d: 5 -> %.1f mm at v=%.1f, a_max=a_stop=%.0f; hold 3 s -> 5 mm\n",
                trial, target_mm, velocity_mm_s, acceleration_mm_s2);
    ctx.current_target_mm = target_mm;
    ctx.current_velocity_mm_s = velocity_mm_s;
    ctx.current_acceleration_mm_s2 = acceleration_mm_s2;
    ctx.current_stop_acceleration_mm_s2 = acceleration_mm_s2;
    if (!configureTrap(acceleration_mm_s2, acceleration_mm_s2)) return false;
    if (!moveAndSample(ctx, trial, "move_forward", target_mm, velocity_mm_s)) return false;
    if (!sampleForDuration(ctx, trial, "hold_target", FORMAL_HOLD_S,
                           target_mm, 0.0, IDLE_SAMPLE_SLEEP_MS)) return false;
    ctx.current_target_mm = X_START_MM;
    ctx.current_velocity_mm_s = REFERENCE_VELOCITY_MM_S;
    ctx.current_acceleration_mm_s2 = REFERENCE_ACCELERATION_MM_S2;
    ctx.current_stop_acceleration_mm_s2 = REFERENCE_ACCELERATION_MM_S2;
    if (!configureTrap(REFERENCE_ACCELERATION_MM_S2, REFERENCE_ACCELERATION_MM_S2)) return false;
    if (!moveAndSample(ctx, trial, "move_return", X_START_MM,
                       -REFERENCE_VELOCITY_MM_S)) return false;
    return sampleForDuration(ctx, trial, "hold_start", ENDPOINT_HOLD_S,
                             X_START_MM, 0.0, IDLE_SAMPLE_SLEEP_MS);
}

struct FormalItem {
    bool is_reference;
    int index;
    double target_mm;
    double velocity_mm_s;
    double acceleration_mm_s2;
};

static std::vector<FormalItem> buildFormalSequence() {
    std::vector<FormalItem> items;
    std::mt19937 rng(RANDOM_SEED);
    int trial_index = 1;
    int reference_index = 1;
    items.push_back({true, reference_index++, X_REFERENCE_TARGET_MM,
                     REFERENCE_VELOCITY_MM_S, REFERENCE_ACCELERATION_MM_S2});
    for (int block_index = 0; block_index < 5; ++block_index) {
        std::vector<FormalItem> conditions = {
            {false, 0, 22.5, 40.0, 750.0},
            {false, 0, 22.5, 80.0, 1750.0},
            {false, 0, 37.5, 60.0, 1250.0},
            {false, 0, 37.5, 80.0, 750.0},
            {false, 0, 57.5, 40.0, 1750.0},
            {false, 0, 57.5, 80.0, 1250.0},
            {false, 0, 72.5, 40.0, 1250.0},
            {false, 0, 72.5, 60.0, 1750.0},
            {false, 0, 87.5, 60.0, 750.0},
            {false, 0, 87.5, 80.0, 1250.0}
        };
        std::shuffle(conditions.begin(), conditions.end(), rng);
        for (FormalItem item : conditions) {
            item.index = trial_index++;
            items.push_back(item);
        }
        items.push_back({true, reference_index++, X_REFERENCE_TARGET_MM,
                         REFERENCE_VELOCITY_MM_S, REFERENCE_ACCELERATION_MM_S2});
    }
    return items;
}
static std::string jsonEscape(const std::string& value) {
    std::string out;
    for (char ch : value) {
        if (ch == '\\' || ch == '"') out.push_back('\\');
        out.push_back(ch);
    }
    return out;
}

static bool writeManifest(const std::string& path, const std::string& csv_path,
                          const std::string& warmup_csv_path, long long warmup_row_count,
                          const char* xml_file, const RunContext& ctx, bool completed) {
    if (fileExists(path)) return false;
    FILE* fp = NULL;
    fopen_s(&fp, path.c_str(), "w");
    if (!fp) return false;
    const std::string csv_json = jsonEscape(csv_path);
    const std::string warmup_json = jsonEscape(warmup_csv_path);
    const std::string xml_json = jsonEscape(xml_file);
    std::fprintf(fp, "{\n");
    std::fprintf(fp, "  \"package_version\": \"%s\",\n", PACKAGE_VERSION);
    std::fprintf(fp, "  \"experiment\": \"E6-A\",\n");
    std::fprintf(fp, "  \"stage\": \"same_day_blind_combination_validation\",\n");
    std::fprintf(fp, "  \"validation_policy\": \"strict_blind_no_refit_no_model_selection_no_filter_retuning\",\n");
    std::fprintf(fp, "  \"session_id\": \"%s\",\n", ctx.session_id.c_str());
    std::fprintf(fp, "  \"csv_path\": \"%s\",\n", csv_json.c_str());
    std::fprintf(fp, "  \"warmup_csv_path\": \"%s\",\n", warmup_json.c_str());
    std::fprintf(fp, "  \"net_xml_file\": \"%s\",\n", xml_json.c_str());
    std::fprintf(fp, "  \"frozen_model_version\": \"%s\",\n", FROZEN_MODEL_VERSION);
    std::fprintf(fp, "  \"frozen_model_sha256\": \"%s\",\n", FROZEN_MODEL_SHA256);
    std::fprintf(fp, "  \"frozen_stop_history_interpolation\": \"piecewise bilinear in v_entry and a_stop inside E5 grid; constant inside hold windows; g_stop assumed position-separable from f_x_hold\",\n");
    std::fprintf(fp, "  \"core\": %d, \"axis\": %d,\n", CORE, AXIS);
    std::fprintf(fp, "  \"controller_zero_mm\": %.3f, \"trajectory_start_mm\": %.3f, \"blind_target_max_mm\": %.3f, \"reference_target_mm\": %.3f,\n",
                 X_ZERO_MM, X_START_MM, X_TARGET_MM, X_REFERENCE_TARGET_MM);
    std::fprintf(fp, "  \"contact_config\": \"%s\",\n", CONTACT_CONFIG);
    std::fprintf(fp, "  \"blind_matrix\": {\"targets_mm\": [22.5,37.5,57.5,72.5,87.5], \"velocities_mm_s\": [40,60,80], \"accelerations_mm_s2\": [750,1250,1750], \"selected_unique_combinations\": 10, \"repeats_per_combination\": 5, \"a_max_equals_a_stop\": true},\n");
    std::fprintf(fp, "  \"standard_reference\": {\"target_mm\": %.1f, \"velocity_mm_s\": %.1f, \"a_max_mm_s2\": %.1f, \"a_stop_mm_s2\": %.1f, \"hold_s\": %.1f},\n",
                 X_REFERENCE_TARGET_MM, REFERENCE_VELOCITY_MM_S,
                 REFERENCE_ACCELERATION_MM_S2, REFERENCE_ACCELERATION_MM_S2,
                 FORMAL_HOLD_S);
    std::fprintf(fp, "  \"trap_smooth_time_ms\": %d, \"blind_hold_s\": %.3f, \"return_hold_s\": %.3f,\n",
                 TRAP_SMOOTH_TIME_MS, FORMAL_HOLD_S, ENDPOINT_HOLD_S);
    std::fprintf(fp, "  \"phase_metrics_required\": [\"acceleration\",\"stable_motion\",\"deceleration\",\"early_hold\",\"late_hold\",\"free_calibration\"],\n");
    std::fprintf(fp, "  \"randomization\": \"five randomized complete ten-condition blocks; one standard reference before block 1 and after every block\", \"random_seed\": %u,\n",
                 RANDOM_SEED);
    std::fprintf(fp, "  \"warmup_count\": %d, \"e6a_trial_count\": %d, \"reference_count\": %d,\n",
                 WARMUP_COUNT, E6A_TRIAL_COUNT, E0_REFERENCE_COUNT);
    std::fprintf(fp, "  \"total_trajectory_count\": %d, \"warmup_period_s\": %.3f, \"formal_period_s\": %.3f,\n",
                 TOTAL_TRAJECTORY_COUNT, WARMUP_PERIOD_S, FORMAL_PERIOD_S);
    const std::vector<FormalItem> formal_items = buildFormalSequence();
    std::fprintf(fp, "  \"formal_sequence\": [");
    for (size_t i = 0; i < formal_items.size(); ++i) {
        const FormalItem& item = formal_items[i];
        if (i > 0) std::fprintf(fp, ",");
        if (item.is_reference) {
            std::fprintf(fp, "\"ref_%02d\"", item.index);
        } else {
            std::fprintf(fp, "\"e6a_trial_%02d_x%04d_v%03d_a%04d\"", item.index,
                         static_cast<int>(item.target_mm * 10.0 + 0.5),
                         static_cast<int>(item.velocity_mm_s + 0.5),
                         static_cast<int>(item.acceleration_mm_s2 + 0.5));
        }
    }
    std::fprintf(fp, "],\n");
    std::fprintf(fp, "  \"current_conversion\": {\"rated_Arms\": %.6f, \"raw_full_scale\": %.3f, \"sign\": %.1f},\n",
                 IFB_RATED_CURRENT_ARMS, ATL_TORQUE_FULL_SCALE, CURRENT_SIGN);
    std::fprintf(fp, "  \"warmup_row_count\": %lld, \"formal_row_count\": %lld,\n",
                 warmup_row_count, ctx.row_count);
    std::fprintf(fp, "  \"completed\": %s, \"safety_stop\": %s, \"api_failure\": %s,\n",
                 completed ? "true" : "false",
                 ctx.safety_stop ? "true" : "false",
                 ctx.api_failure ? "true" : "false");
    std::fprintf(fp, "  \"note\": \"Blind validation only. Do not use E6-A to refit E0-E5 terms, choose model complexity, alter lookup nodes, or retune filtering. Estimate delta_b_trial only from confirmed no-contact free motion during analysis; keep E6-A permanently separate from identification data.\"\n");
    std::fprintf(fp, "}\n");
    std::fclose(fp);
    return true;
}
static void printUsage(const char* exe) {
    std::printf("Usage: %s [xml_file] [output_root]\n", exe);
}

int main(int argc, char* argv[]) {
    timeBeginPeriod(1);
    const char* xml_file = DEFAULT_NET_XML_FILE;
    std::string output_root = DEFAULT_OUTPUT_ROOT;
    if (argc >= 2) {
        if (!std::strcmp(argv[1], "-h") || !std::strcmp(argv[1], "--help") || !std::strcmp(argv[1], "/?")) {
            printUsage(argv[0]);
            timeEndPeriod(1);
            return 0;
        }
        xml_file = argv[1];
    }
    if (argc >= 3) output_root = argv[2];
    if (!ensurePackageDirs(output_root)) {
        std::printf("Cannot create output directories under %s\n", output_root.c_str());
        timeEndPeriod(1);
        return -1;
    }

    RunContext ctx = {};
    const std::vector<FormalItem> formal_items = buildFormalSequence();

    ctx.session_id = makeSessionId();
    const std::string csv_path = output_root + "/data/raw/e6a_blind_validation_" + ctx.session_id + ".csv";
    const std::string warmup_csv_path = output_root + "/data/raw/e6a_formal_warmup_" + ctx.session_id + ".csv";
    const std::string manifest_path = output_root + "/manifest/e6a_blind_validation_" + ctx.session_id + ".json";
    if (fileExists(csv_path) || fileExists(warmup_csv_path) || fileExists(manifest_path)) {
        std::printf("Output collision; refusing to overwrite existing files.\n");
        timeEndPeriod(1);
        return -1;
    }

    std::printf("E6-A same-day blind combination validation\n");
    std::printf("%d warm-up trajectories at %.0f s, then %d E6-A blind trials + %d references at %.0f s start-to-start.\n",
                WARMUP_COUNT, WARMUP_PERIOD_S, E6A_TRIAL_COUNT,
                E0_REFERENCE_COUNT, FORMAL_PERIOD_S);
    std::printf("Blind intermediate values: target=22.5/37.5/57.5/72.5/87.5 mm, v=40/60/80, a=750/1250/1750.\n");
    std::printf("Five randomized complete ten-condition blocks, 10 unique combinations, hold=3 s, trap smooth=%d ms.\n", TRAP_SMOOTH_TIME_MS);
    std::printf("Frozen v3 blind validation: never refit or retune with E6-A data; analyze all phases separately.\n");
    std::printf("Physically place the axis at its established x=0 mm zero position.\n");
    std::printf("The program will set controller zero there, then move to x=5 mm before logging.\n");
    std::printf("Verify the full 0-87.5 mm path is contact-free and keep E-stop reachable.\n");
    std::printf("Type NO_CONTACT then Enter to arm: ");
    char confirmation[64] = {};
    if (!std::fgets(confirmation, sizeof(confirmation), stdin) ||
        std::strncmp(confirmation, "NO_CONTACT", 10) != 0) {
        std::printf("Not armed.\n");
        timeEndPeriod(1);
        return 0;
    }

    short ret = 0;
    long net_status = 0;
    const long mask = 1L << (AXIS - 1);
    bool card_opened = false, axis_on = false, ok = true, completed = false;
    long long warmup_row_count = 0;
    ret = GTN_OpenCard();
    if (ret != 0) { commandHandler("GTN_OpenCard", ret); ok = false; goto cleanup; }
    card_opened = true;
    ret = GTN_NetInit(NET_INIT_MODE_XML_STRICT, const_cast<char*>(xml_file), 60, &net_status);
    if (ret != 0) { std::printf("GTN_NetInit failed: %d status=%ld\n", ret, net_status); ok = false; goto cleanup; }
    ret = GTN_ClrSts(CORE, AXIS, 1); if (ret != 0) { commandHandler("GTN_ClrSts", ret); ok = false; goto cleanup; }
    ret = GTN_AxisOn(CORE, AXIS); if (ret != 0) { commandHandler("GTN_AxisOn", ret); ok = false; goto cleanup; }
    axis_on = true;
    Sleep(500);

    {
        long status = 0;
        short lp = 0, ln = 0;
        if (GTN_GetSts(CORE, AXIS, &status, 1, NULL) != 0 ||
            GTN_GetLimitStatus(CORE, AXIS, &lp, &ln) != 0 || isAxisFault(status) || lp || ln) {
            std::printf("Initial servo/fault/limit check failed.\n");
            ok = false;
            goto cleanup;
        }
    }

    ret = GTN_ZeroPos(CORE, AXIS, 1); if (ret != 0) { commandHandler("GTN_ZeroPos", ret); ok = false; goto cleanup; }
    ret = GTN_SetPrfPos(CORE, AXIS, 0); if (ret != 0) { commandHandler("GTN_SetPrfPos", ret); ok = false; goto cleanup; }
    ret = GTN_SetSoftLimitMode(CORE, AXIS, LIMIT_MODE_SOFT); if (ret != 0) { commandHandler("GTN_SetSoftLimitMode", ret); ok = false; goto cleanup; }
    ret = GTN_SetSoftLimit(CORE, AXIS, physicalMmToPulse(SOFT_LIMIT_HIGH_MM), physicalMmToPulse(SOFT_LIMIT_LOW_MM));
    if (ret != 0) { commandHandler("GTN_SetSoftLimit", ret); ok = false; goto cleanup; }
    ret = GTN_PrfTrap(CORE, AXIS); if (ret != 0) { commandHandler("GTN_PrfTrap", ret); ok = false; goto cleanup; }
    if (!configureTrap(REFERENCE_ACCELERATION_MM_S2, REFERENCE_ACCELERATION_MM_S2)) { ok = false; goto cleanup; }
    for (const FormalItem& spec : formal_items) {
        if (!spec.is_reference && spec.velocity_mm_s * spec.velocity_mm_s / spec.acceleration_mm_s2 > spec.target_mm - X_START_MM) {
            std::printf("Trajectory is not feasible: target=%.1f v=%.1f a=%.1f.\n", spec.target_mm, spec.velocity_mm_s, spec.acceleration_mm_s2);
            ok = false;
            goto cleanup;
        }
    }

    {
        double axis_enc = 0.0, raw_enc = 0.0;
        short ra = GTN_GetAxisEncPos(CORE, AXIS, &axis_enc, 1, NULL);
        short rr = GTN_GetEncPos(CORE, AXIS, &raw_enc, 1, NULL);
        if (ra != 0 && rr != 0) { ok = false; goto cleanup; }
        ctx.zero_axis_enc_pulse = axis_enc;
        ctx.zero_raw_enc_pulse = raw_enc;
    }

    if (!moveToExperimentStart(ctx)) { ok = false; goto cleanup; }

    fopen_s(&ctx.csv, warmup_csv_path.c_str(), "w");
    if (!ctx.csv || !writeCsvHeader(ctx.csv)) { ok = false; goto cleanup; }
    QueryPerformanceFrequency(&ctx.freq);
    QueryPerformanceCounter(&ctx.t0);
    ctx.prev_sample = ctx.t0;
    ctx.unix_t0_s = unixTimeNowSeconds();

    for (int warmup = 1; warmup <= WARMUP_COUNT && ok; ++warmup) {
        setItemMetadata(ctx, "warmup", warmup);
        const double scheduled_s = INITIAL_WAIT_S + (warmup - 1) * WARMUP_PERIOD_S;
        if (!sampleUntilRunTime(ctx, warmup, scheduled_s)) { ok = false; break; }
        LARGE_INTEGER now = {};
        QueryPerformanceCounter(&now);
        std::printf("  warm-up %d/%d scheduled=%.1f s actual=%.3f s\n",
                    warmup, WARMUP_COUNT, scheduled_s, qpcSinceStart(now, ctx));
        if (!runReferenceTrajectory(ctx, warmup)) ok = false;
        warmup_row_count = ctx.row_count;
        std::fflush(ctx.csv);
    }

    if (ctx.csv) {
        std::fflush(ctx.csv);
        std::fclose(ctx.csv);
        ctx.csv = NULL;
    }

    if (ok) {
        ctx.row_count = 0;
        ctx.next_sample_index = 0;
        ctx.have_prev_sample = false;
        ctx.have_acceleration_state = false;
        fopen_s(&ctx.csv, csv_path.c_str(), "w");
        if (!ctx.csv || !writeCsvHeader(ctx.csv)) ok = false;
    }


    if (formal_items.size() != FORMAL_ITEM_COUNT) {
        std::printf("Internal sequence length mismatch.\n");
        ok = false;
    }
    for (size_t item = 0; item < formal_items.size() && ok; ++item) {
        const FormalItem& spec = formal_items[item];
        setItemMetadata(ctx, spec.is_reference ? "reference" : "blind",
                        spec.index, spec.target_mm, spec.velocity_mm_s, spec.acceleration_mm_s2);
        const double formal_start_s = INITIAL_WAIT_S +
            (WARMUP_COUNT - 1) * WARMUP_PERIOD_S + FORMAL_PERIOD_S;
        const double scheduled_s = formal_start_s + static_cast<double>(item) * FORMAL_PERIOD_S;
        if (!sampleUntilRunTime(ctx, spec.index, scheduled_s)) { ok = false; break; }
        LARGE_INTEGER now = {};
        QueryPerformanceCounter(&now);
        std::printf("  formal %zu/%zu %s target=%.1f v=%.1f a=%.0f scheduled=%.1f s actual=%.3f s\n",
                    item + 1, formal_items.size(), ctx.current_reference_id.c_str(),
                    spec.target_mm, spec.velocity_mm_s, spec.acceleration_mm_s2, scheduled_s, qpcSinceStart(now, ctx));
        if (spec.is_reference) {
            if (!runReferenceTrajectory(ctx, spec.index)) ok = false;
        } else {
            if (!runBlindTrial(ctx, spec.index, spec.target_mm, spec.velocity_mm_s, spec.acceleration_mm_s2)) ok = false;
        }
        std::fflush(ctx.csv);
    }

    if (ok && !ctx.safety_stop) {
        double axis_enc = 0.0;
        if (GTN_GetAxisEncPos(CORE, AXIS, &axis_enc, 1, NULL) != 0 ||
            std::fabs(pulseToPhysicalMm(axis_enc - ctx.zero_axis_enc_pulse) - X_START_MM) > START_TOLERANCE_MM) {
            std::printf("Final x=5 mm return check failed.\n");
            ok = false;
            ctx.safety_stop = true;
        }
    }
    completed = ok && !ctx.safety_stop && !ctx.api_failure;

cleanup:
    if (card_opened) {
        GTN_Stop(CORE, mask, (ctx.safety_stop || !ok) ? mask : 0);
        Sleep(300);
        if (axis_on) GTN_AxisOff(CORE, AXIS);
        GTN_Close();
    }
    if (ctx.csv) {
        std::fflush(ctx.csv);
        std::fclose(ctx.csv);
        ctx.csv = NULL;
    }
    writeManifest(manifest_path, csv_path, warmup_csv_path, warmup_row_count,
                  xml_file, ctx, completed);
    std::printf("Warm-up CSV: %s\nIdentification CSV: %s\nManifest: %s\n",
                warmup_csv_path.c_str(), csv_path.c_str(), manifest_path.c_str());
    timeEndPeriod(1);
    if (!completed) {
        std::printf("E6-A blind acquisition stopped/failed. Keep the partial CSV separate and diagnose it; never merge partial data into identification sets.\n");
        return -1;
    }
    std::printf("E6-A blind validation acquisition complete. Keep these data permanently out of training.\n");
    return 0;
}





