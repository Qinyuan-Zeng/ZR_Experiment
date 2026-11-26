#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <conio.h>
#include <windows.h>
#include <mmsystem.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>
#include <algorithm>
#include <random>
#include "gxn.h"

#pragma comment(lib, "gxn.lib")
#pragma comment(lib, "winmm.lib")

#define CORE 2
#define AXIS 1

static const char* PACKAGE_VERSION = "modeling_experiment3_controlled_state_d4_v1_1";
static const char* ACQUISITION_PROTOCOL_VERSION = "d4_acq_v1_1";
static const char* PREPROCESS_VERSION = "m3_preprocess_v1_1";
static const char* TRAINING_VERSION = "m4_train_v1_1";
static const char* SOURCE_GIT_COMMIT = "unavailable";
static const char* EXECUTABLE_BUILD_TIME = __DATE__ " " __TIME__;
static const char* DEFAULT_NET_XML_FILE = "Googol.xml";
static const char* DEFAULT_OUTPUT_ROOT = "E:\\A_ZQY\\Experiment\\ModelingExperiment3";
static const double PULSE_PER_MM = 10000.0;
static const double IFB_RATED_CURRENT_ARMS = 4.500;
static const double ATL_TORQUE_FULL_SCALE = 1000.0;
static const double CURRENT_SIGN = 1.0;

static const double HOME_ZERO_MM = 0.0;
static const double EXPERIMENT_START_MM = 5.0;
static const double WARMUP_FAR_MM = 95.0;
static const double X_MIN_MM = 0.0;
static const double X_MAX_MM = 100.0;
static const double SOFT_LIMIT_LOW_MM = 1.0;
static const double SOFT_LIMIT_HIGH_MM = 99.0;
static const double GUARD_LOW_MM = 1.5;
static const double GUARD_HIGH_MM = 98.5;
static const double START_WAIT_S = 1.5;
static const double HOLD_S = 1.0;
static const double V2_HOLD_S = 5.0;
static const double V2_SWITCH_MM = 70.0;
static const double V2_APPROACH_MM_S = 50.0;
static const double V2_ACCEL_MM_S2 = 200.0;
static const double RETURN_WAIT_S = 1.5;
static const int WARMUP_COUNT = 6;
static const double WARMUP_INITIAL_WAIT_S = 5.0;
static const double WARMUP_PERIOD_S = 60.0;
static const unsigned int BASE_RANDOM_SEED = 20260922;
static const double DRIFT_ENDPOINT_HOLD_S = 0.5;
static const double MOTION_TIMEOUT_EXTRA_S = 6.0;
static const short TRAP_SMOOTH_TIME_MS = 25;
static const DWORD IDLE_SAMPLE_SLEEP_MS = 4;

struct Condition {
    std::string id;
    double target_mm;
    double velocity_mm_s;
    double acceleration_mm_s2;
    int trials;
    std::string validation_role;
    int block_id;
    int planned_trial_index;
};

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
    std::string session_start_time;
    std::string mode;
    std::string active_drift_reference_id;
    unsigned int session_seed;
    int warmup_cycle_index;
    double warmup_scheduled_start_s;
    double warmup_actual_start_s;
    double warmup_actual_wait_s;
    std::string planned_order_path;
    std::string executed_order_path;
    std::string session_config_path;
    std::string session_summary_path;
    FILE* csv;
    FILE* executed_order;
    std::vector<short> active_reference_samples;
    bool first_reference_available;
    double first_reference_raw;
    double drift_causal_raw;
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
    return static_cast<long>((mm >= 0.0) ? (mm * PULSE_PER_MM + 0.5) : (mm * PULSE_PER_MM - 0.5));
}

static long physicalMmToPulse(double physical_mm) {
    return mmToPulseLong(physical_mm - EXPERIMENT_START_MM);
}

static double pulseToMm(double pulse) { return pulse / PULSE_PER_MM; }
static double pulseToPhysicalMm(double pulse) { return EXPERIMENT_START_MM + pulseToMm(pulse); }
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
    return ensureDirectory(root) && ensureDirectory(root + "\\acquisition") &&
           ensureDirectory(root + "\\config") && ensureDirectory(root + "\\data") &&
           ensureDirectory(root + "\\data\\raw") && ensureDirectory(root + "\\manifest") &&
           ensureDirectory(root + "\\scripts") && ensureDirectory(root + "\\results");
}

static bool fileExists(const std::string& path) {
    DWORD attr = GetFileAttributesA(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static std::string makeSessionId() {
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    char buf[64] = {};
    sprintf_s(buf, sizeof(buf), "%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return std::string(buf);
}

static std::string makeSessionStartTime() {
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    char buf[64] = {};
    sprintf_s(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03d",
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return std::string(buf);
}

static unsigned int stableSessionSeed(unsigned int base_seed, const std::string& session_id) {
    unsigned int hash = 2166136261u;
    for (unsigned char ch : session_id) {
        hash ^= static_cast<unsigned int>(ch);
        hash *= 16777619u;
    }
    return base_seed ^ hash;
}

static std::string jsonEscape(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 16);
    static const char* hex = "0123456789abcdef";
    for (unsigned char ch : value) {
        switch (ch) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (ch < 0x20) {
                out += "\\u00";
                out += hex[(ch >> 4) & 0x0f];
                out += hex[ch & 0x0f];
            } else {
                out += static_cast<char>(ch);
            }
        }
    }
    return out;
}

static bool isAxisFault(long s) { return ((s & (1 << 1)) || (s & (1 << 5)) || (s & (1 << 6))); }
static bool isMoving(long s) { return (s & 0x400) != 0; }

static bool userRequestedStop() {
    if (!_kbhit()) return false;
    int ch = _getch();
    return ch == 'q' || ch == 'Q';
}

static const char* directionForPhase(const char* phase) {
    if (std::strcmp(phase, "move_forward") == 0 || std::strcmp(phase, "drift_move_forward") == 0 || std::strcmp(phase, "FAST_APPROACH") == 0 || std::strcmp(phase, "PRESS") == 0) return "positive";
    if (std::strcmp(phase, "move_return") == 0 || std::strcmp(phase, "drift_move_return") == 0 || std::strcmp(phase, "RETURN") == 0) return "negative";
    return "stationary";
}

static const char* trajectoryType(double start_mm, double target_mm, double v_mm_s, double a_mm_s2) {
    const double stroke = std::fabs(target_mm - start_mm);
    const double s_accel = v_mm_s * v_mm_s / (2.0 * maxDouble(a_mm_s2, 1e-9));
    return (2.0 * s_accel <= stroke) ? "trapezoid" : "triangular";
}

static const char* recordTypeForRole(const std::string& role) {
    if (role == "warmup") return "warmup";
    if (role == "drift_reference") return "drift_reference";
    return "formal";
}

static bool isV2(const std::string& mode) {
    return mode == "v2_controlled" || mode == "v2_blind" || mode == "v2_mini";
}

static const char* protocolVersion(const std::string& mode) {
    return isV2(mode) ? "v2_segmented_acq_v1" : ACQUISITION_PROTOCOL_VERSION;
}

static bool writeCsvHeader(FILE* fp) {
    if (!fp) return false;
    std::fprintf(fp,
        "timestamp_unix_s,time_s,run_time,session_id,dataset_mode,trial_id,condition_id,phase,phase_time,"
        "reference_id,drift_reference_id,model_version,validation_role,"
        "record_type,quality_status,acquisition_protocol_version,source_git_commit,executable_build_time,session_start_time,"
        "session_seed,block_id,planned_trial_index,warmup_cycle_index,warmup_scheduled_start_s,warmup_actual_start_s,warmup_actual_wait_s,"
        "x_cmd_mm,x_act_mm,x_prf_mm,v_cmd_mm_s,v_act_mm_s,v_prf_mm_s,a_cmd_mm_s2,a_prf_mm_s2,a_est_mm_s2,"
        "current_raw,current_A,current_command,target_mm,v_max_mm_s,a_max_mm_s2,a_stop_mm_s2,hold_time_s,"
        "direction,contact_config,position_error_mm,velocity_error_mm_s,servo_state,temperature,"
        "trajectory_type,target_minus_x_mm,progress,normalized_phase,time_since_stop_s,"
        "safety_stop,api_failure,sample_index,phase_sample_index,dt_s,api_duration_us,planner_status,"
        "limit_positive,limit_negative,enc_source,trap_smooth_time_ms,v_press_mm_s,v_approach_mm_s,x_switch_mm,condition_repeat_index,a_cmd_causal_mm_s2,drift_causal_raw,current_drift_corrected_raw\n");
    return std::ferror(fp) == 0;
}

static bool readAndWriteSample(RunContext& ctx, const Condition& cond, int trial_id, const char* phase,
                               long phase_index, double phase_start_s, double x_cmd_mm,
                               double v_cmd_mm_s, const char* reference_id, const char* contact_config) {
    if (!ctx.csv) return false;
    LARGE_INTEGER sample_start = {}, api_start = {}, api_end = {};
    QueryPerformanceCounter(&sample_start);
    QueryPerformanceCounter(&api_start);

    double dt_s = 0.0;
    if (ctx.have_prev_sample) dt_s = qpcSeconds(sample_start, ctx.freq) - qpcSeconds(ctx.prev_sample, ctx.freq);
    else ctx.have_prev_sample = true;
    ctx.prev_sample = sample_start;
    const double run_time_s = qpcSinceStart(sample_start, ctx);
    const double phase_time_s = run_time_s - phase_start_s;

    long axis_status = 0, planner_status = 0;
    double prf_pos = 0.0, prf_vel = 0.0, axis_enc = 0.0, raw_enc = 0.0;
    double axis_enc_vel = 0.0, axis_error = 0.0, axis_prf_acc = 0.0;
    short lim_pos = 0, lim_neg = 0, current_raw = 0;
    short r_sts = GTN_GetSts(CORE, AXIS, &axis_status, 1, NULL);
    short r_prf_sts = GTN_GetPrfSts(CORE, AXIS, &planner_status, 1, NULL);
    short r_prf_pos = GTN_GetPrfPos(CORE, AXIS, &prf_pos, 1, NULL);
    short r_prf_vel = GTN_GetPrfVel(CORE, AXIS, &prf_vel, 1, NULL);
    short r_axis_pos = GTN_GetAxisEncPos(CORE, AXIS, &axis_enc, 1, NULL);
    short r_raw_pos = GTN_GetEncPos(CORE, AXIS, &raw_enc, 1, NULL);
    short r_axis_vel = GTN_GetAxisEncVel(CORE, AXIS, &axis_enc_vel, 1, NULL);
    short r_prf_acc = GTN_GetAxisPrfAcc(CORE, AXIS, &axis_prf_acc, 1, NULL);
    short r_error = GTN_GetAxisError(CORE, AXIS, &axis_error, 1, NULL);
    short r_limit = GTN_GetLimitStatus(CORE, AXIS, &lim_pos, &lim_neg);
    short r_current = GTN_GetAtlTorque(CORE, AXIS, &current_raw);
    QueryPerformanceCounter(&api_end);

    const bool api_failure = r_sts || r_prf_sts || r_prf_pos || r_prf_vel || (r_axis_pos && r_raw_pos) ||
                             r_axis_vel || r_prf_acc || r_error || r_limit || r_current;
    if (api_failure) ctx.api_failure = true;

    double x_act = std::numeric_limits<double>::quiet_NaN();
    const char* enc_source = "none";
    if (r_axis_pos == 0) {
        x_act = EXPERIMENT_START_MM + pulseToMm(axis_enc - ctx.zero_axis_enc_pulse);
        enc_source = "axis";
    } else if (r_raw_pos == 0) {
        x_act = EXPERIMENT_START_MM + pulseToMm(raw_enc - ctx.zero_raw_enc_pulse);
        enc_source = "raw";
    }
    const double v_act = pulseToMm(axis_enc_vel * 1000.0);
    const double x_prf = pulseToPhysicalMm(prf_pos);
    const double v_prf = pulseToMm(prf_vel * 1000.0);
    const double a_prf = r_prf_acc == 0 ? pulseToMm(axis_prf_acc * 1000000.0) : std::numeric_limits<double>::quiet_NaN();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double position_error_mm = pulseToMm(axis_error);
    const double velocity_error_mm_s = v_cmd_mm_s - v_act;
    if (isV2(ctx.mode) && cond.validation_role == "drift_reference" &&
        std::strcmp(phase, "drift_move_forward") == 0 && x_act >= 30.0 && x_act <= 60.0)
        ctx.active_reference_samples.push_back(current_raw);
    const double target_minus_x = cond.target_mm - x_act;
    const double travel = cond.target_mm - EXPERIMENT_START_MM;
    double progress = (std::fabs(travel) > 1e-9) ? (x_act - EXPERIMENT_START_MM) / travel : nan;
    if (isV2(ctx.mode) && std::strcmp(phase, "FAST_APPROACH") == 0)
        progress = (x_act - EXPERIMENT_START_MM) / (V2_SWITCH_MM - EXPERIMENT_START_MM);
    if (isV2(ctx.mode) && std::strcmp(phase, "PRESS") == 0)
        progress = (x_act - V2_SWITCH_MM) / (cond.target_mm - V2_SWITCH_MM);
    const double hold_s = isV2(ctx.mode) ? V2_HOLD_S : HOLD_S;
    const double normalized_phase = phase_time_s / maxDouble(hold_s, 1e-9);
    const double time_since_stop = (std::strcmp(phase, "hold_at_target") == 0 || std::strcmp(phase, "HOLD") == 0) ? phase_time_s : nan;

    if (api_failure || isAxisFault(axis_status) || lim_pos || lim_neg || !std::isfinite(x_act) ||
        x_act < GUARD_LOW_MM || x_act > GUARD_HIGH_MM) ctx.safety_stop = true;

    const double api_us = (qpcSeconds(api_end, ctx.freq) - qpcSeconds(api_start, ctx.freq)) * 1e6;
    std::fprintf(ctx.csv,
        "%.9f,%.9f,%.9f,%s,%s,%d,%s,%s,%.9f,%s,%s,%s,%s,"
        "%s,%s,%s,%s,%s,%s,%u,%d,%d,%d,%.9f,%.9f,%.9f,"
        "%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,"
        "%d,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,"
        "%s,%s,%.9f,%.9f,0x%08lX,%.9f,%s,%.9f,%.9f,%.9f,%.9f,"
        "%d,%d,%lld,%ld,%.9f,%.3f,0x%08lX,%d,%d,%s,%d,%.9f,%.9f,%.9f,%d,%.9f,%.9f,%.9f\n",
        ctx.unix_t0_s + run_time_s, run_time_s, run_time_s, ctx.session_id.c_str(), ctx.mode.c_str(), trial_id,
        cond.id.c_str(), phase, phase_time_s, reference_id, ctx.active_drift_reference_id.c_str(), PACKAGE_VERSION, cond.validation_role.c_str(),
        recordTypeForRole(cond.validation_role), (api_failure || ctx.safety_stop) ? "invalid" : "ok",
        protocolVersion(ctx.mode), SOURCE_GIT_COMMIT, EXECUTABLE_BUILD_TIME, ctx.session_start_time.c_str(),
        ctx.session_seed, cond.block_id, cond.planned_trial_index, ctx.warmup_cycle_index,
        ctx.warmup_scheduled_start_s, ctx.warmup_actual_start_s, ctx.warmup_actual_wait_s,
        x_cmd_mm, x_act, x_prf, v_cmd_mm_s, v_act, v_prf, cond.acceleration_mm_s2, a_prf, nan,
        current_raw, currentRawToArms(current_raw), nan, cond.target_mm, cond.velocity_mm_s, cond.acceleration_mm_s2, cond.acceleration_mm_s2, hold_s,
        directionForPhase(phase), contact_config, position_error_mm, velocity_error_mm_s, axis_status, nan,
        trajectoryType(EXPERIMENT_START_MM, cond.target_mm, cond.velocity_mm_s, cond.acceleration_mm_s2), target_minus_x, progress, normalized_phase, time_since_stop,
        ctx.safety_stop ? 1 : 0, api_failure ? 1 : 0, ctx.next_sample_index++, phase_index, dt_s, api_us, planner_status,
        lim_pos, lim_neg, enc_source, TRAP_SMOOTH_TIME_MS,
        isV2(ctx.mode) ? cond.velocity_mm_s : nan, isV2(ctx.mode) ? V2_APPROACH_MM_S : nan,
        isV2(ctx.mode) ? V2_SWITCH_MM : nan, isV2(ctx.mode) ? cond.block_id : 0,
        isV2(ctx.mode) ? a_prf : nan,
        isV2(ctx.mode) && ctx.first_reference_available ? ctx.drift_causal_raw : nan,
        isV2(ctx.mode) && ctx.first_reference_available ? current_raw - ctx.drift_causal_raw : nan);
    if (std::ferror(ctx.csv)) {
        ctx.api_failure = true;
        ctx.safety_stop = true;
    }
    ++ctx.row_count;
    return !ctx.safety_stop;
}

static bool sampleForDuration(RunContext& ctx, const Condition& cond, int trial_id, const char* phase,
                              double duration_s, double x_cmd, double v_cmd, const char* reference_id,
                              const char* contact_config) {
    LARGE_INTEGER start = {};
    QueryPerformanceCounter(&start);
    const double phase_start_s = qpcSinceStart(start, ctx);
    long phase_index = 0;
    while (true) {
        if (userRequestedStop()) ctx.safety_stop = true;
        if (!readAndWriteSample(ctx, cond, trial_id, phase, phase_index++, phase_start_s, x_cmd, v_cmd, reference_id, contact_config)) return false;
        LARGE_INTEGER now = {};
        QueryPerformanceCounter(&now);
        if (qpcSeconds(now, ctx.freq) - qpcSeconds(start, ctx.freq) >= duration_s) return true;
        if (IDLE_SAMPLE_SLEEP_MS > 0) Sleep(IDLE_SAMPLE_SLEEP_MS);
    }
}

static bool configureTrap(const Condition& cond) {
    TTrapPrm trap = {};
    short ret = GTN_GetTrapPrm(CORE, AXIS, &trap);
    commandHandler("GTN_GetTrapPrm", ret);
    if (ret != 0) return false;
    trap.acc = cond.acceleration_mm_s2 * PULSE_PER_MM / 1000000.0;
    trap.dec = cond.acceleration_mm_s2 * PULSE_PER_MM / 1000000.0;
    trap.velStart = 0.0;
    trap.smoothTime = TRAP_SMOOTH_TIME_MS;
    ret = GTN_SetTrapPrm(CORE, AXIS, &trap);
    commandHandler("GTN_SetTrapPrm", ret);
    return ret == 0;
}

static bool moveAndSample(RunContext& ctx, const Condition& cond, int trial_id, const char* phase,
                          double target_mm, double signed_v_mm_s, const char* reference_id,
                          const char* contact_config) {
    if (target_mm < X_MIN_MM || target_mm > X_MAX_MM) return false;
    short ret = GTN_SetPos(CORE, AXIS, physicalMmToPulse(target_mm));
    commandHandler("GTN_SetPos", ret);
    if (ret != 0) return false;
    ret = GTN_SetVel(CORE, AXIS, std::fabs(signed_v_mm_s) * PULSE_PER_MM / 1000.0);
    commandHandler("GTN_SetVel", ret);
    if (ret != 0) return false;
    ret = GTN_Update(CORE, 1L << (AXIS - 1));
    commandHandler("GTN_Update", ret);
    if (ret != 0) return false;

    const double travel = std::fabs(target_mm - EXPERIMENT_START_MM);
    const double timeout_s = maxDouble(8.0, travel / maxDouble(std::fabs(signed_v_mm_s), 1.0) +
                                       std::fabs(signed_v_mm_s) / maxDouble(cond.acceleration_mm_s2, 1.0) +
                                       MOTION_TIMEOUT_EXTRA_S);
    LARGE_INTEGER start = {};
    QueryPerformanceCounter(&start);
    const double phase_start_s = qpcSinceStart(start, ctx);
    long phase_index = 0;
    while (true) {
        if (userRequestedStop()) ctx.safety_stop = true;
        if (!readAndWriteSample(ctx, cond, trial_id, phase, phase_index++, phase_start_s, target_mm, signed_v_mm_s, reference_id, contact_config)) return false;
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
            std::printf("Motion timeout: condition=%s trial=%d phase=%s\n", cond.id.c_str(), trial_id, phase);
            ctx.safety_stop = true;
            return false;
        }
    }
}

static bool sampleUntilRunTime(RunContext& ctx, const Condition& cond, int trial_id,
                               const char* phase, double target_run_time_s) {
    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);
    if (qpcSinceStart(now, ctx) >= target_run_time_s) return true;
    const double phase_start_s = qpcSinceStart(now, ctx);
    long phase_index = 0;
    while (true) {
        if (userRequestedStop()) ctx.safety_stop = true;
        if (!readAndWriteSample(ctx, cond, trial_id, phase, phase_index++, phase_start_s,
                                EXPERIMENT_START_MM, 0.0, "none", "no_contact_confirmed")) return false;
        QueryPerformanceCounter(&now);
        if (qpcSinceStart(now, ctx) >= target_run_time_s) return true;
        if (IDLE_SAMPLE_SLEEP_MS > 0) Sleep(IDLE_SAMPLE_SLEEP_MS);
    }
}

static bool runDriftReference(RunContext& ctx, int ref_index, const char* label) {
    Condition ref = {"m3_drift_ref_x95_v50", WARMUP_FAR_MM, 50.0,
                     isV2(ctx.mode) ? V2_ACCEL_MM_S2 : 1000.0, 1, "drift_reference", 0, 0};
    char ref_id[64] = {};
    sprintf_s(ref_id, sizeof(ref_id), "%s_%02d", label, ref_index);
    ctx.active_drift_reference_id = ref_id;
    if (isV2(ctx.mode)) ctx.active_reference_samples.clear();
    std::printf("Drift reference %s: 5 -> 95 -> 5 mm\n", ref_id);
    if (!configureTrap(ref)) return false;
    if (!moveAndSample(ctx, ref, ref_index, "drift_move_forward", WARMUP_FAR_MM, ref.velocity_mm_s, ref_id, "no_contact_confirmed")) return false;
    if (!sampleForDuration(ctx, ref, ref_index, "drift_hold_target", DRIFT_ENDPOINT_HOLD_S, WARMUP_FAR_MM, 0.0, ref_id, "no_contact_confirmed")) return false;
    if (!moveAndSample(ctx, ref, ref_index, "drift_move_return", EXPERIMENT_START_MM, -ref.velocity_mm_s, ref_id, "no_contact_confirmed")) return false;
    if (!sampleForDuration(ctx, ref, ref_index, "drift_hold_start", DRIFT_ENDPOINT_HOLD_S, EXPERIMENT_START_MM, 0.0, ref_id, "no_contact_confirmed")) return false;
    if (isV2(ctx.mode)) {
        if (ctx.active_reference_samples.size() < 5) return false;
        std::sort(ctx.active_reference_samples.begin(), ctx.active_reference_samples.end());
        const size_t n = ctx.active_reference_samples.size();
        const double median = n % 2 ? ctx.active_reference_samples[n / 2] :
            0.5 * (ctx.active_reference_samples[n / 2 - 1] + ctx.active_reference_samples[n / 2]);
        if (!ctx.first_reference_available) {
            ctx.first_reference_raw = median;
            ctx.first_reference_available = true;
        }
        ctx.drift_causal_raw = median - ctx.first_reference_raw;
    }
    return true;
}

static bool runTrial(RunContext& ctx, const Condition& cond, int trial_id) {
    if (!configureTrap(cond)) return false;
    if (!sampleForDuration(ctx, cond, trial_id, "wait_start_pre", START_WAIT_S, EXPERIMENT_START_MM, 0.0, "none", "no_contact_confirmed")) return false;
    if (isV2(ctx.mode)) {
        if (!moveAndSample(ctx, cond, trial_id, "FAST_APPROACH", V2_SWITCH_MM, V2_APPROACH_MM_S, "none", "no_contact_confirmed")) return false;
        if (!moveAndSample(ctx, cond, trial_id, "PRESS", cond.target_mm, cond.velocity_mm_s, "none", "no_contact_confirmed")) return false;
        if (!sampleForDuration(ctx, cond, trial_id, "HOLD", V2_HOLD_S, cond.target_mm, 0.0, "none", "no_contact_confirmed")) return false;
        if (!moveAndSample(ctx, cond, trial_id, "RETURN", EXPERIMENT_START_MM, -V2_APPROACH_MM_S, "none", "no_contact_confirmed")) return false;
        return sampleForDuration(ctx, cond, trial_id, "wait_start_post", RETURN_WAIT_S, EXPERIMENT_START_MM, 0.0, "none", "no_contact_confirmed");
    }
    if (!moveAndSample(ctx, cond, trial_id, "move_forward", cond.target_mm, cond.velocity_mm_s, "none", "no_contact_confirmed")) return false;
    if (!sampleForDuration(ctx, cond, trial_id, "hold_at_target", HOLD_S, cond.target_mm, 0.0, "none", "no_contact_confirmed")) return false;
    if (!moveAndSample(ctx, cond, trial_id, "move_return", EXPERIMENT_START_MM, -cond.velocity_mm_s, "none", "no_contact_confirmed")) return false;
    return sampleForDuration(ctx, cond, trial_id, "wait_start_post", RETURN_WAIT_S, EXPERIMENT_START_MM, 0.0, "none", "no_contact_confirmed");
}

static void addGrid(std::vector<Condition>& out, const char* prefix, const double* xs, int nx,
                    const double* vs, int nv, const double* as, int na, int trials, const char* role) {
    for (int ix = 0; ix < nx; ++ix) {
        for (int iv = 0; iv < nv; ++iv) {
            for (int ia = 0; ia < na; ++ia) {
                char id[96] = {};
                sprintf_s(id, sizeof(id), "%s_x%.0f_v%.0f_a%.0f", prefix, xs[ix], vs[iv], as[ia]);
                out.push_back({id, xs[ix], vs[iv], as[ia], trials, role, 0, 0});
            }
        }
    }
}

static std::vector<Condition> buildConditions(const std::string& mode, unsigned int session_seed) {
    std::vector<Condition> c;
    if (isV2(mode)) {
        const double train_xs[] = {75.0, 80.0, 85.0, 90.0};
        const double train_vs[] = {5.0, 10.0, 15.0, 20.0};
        const double blind_xs[] = {80.0, 90.0};
        const double blind_vs[] = {7.5, 12.5, 17.5};
        std::vector<Condition> base;
        const int nx = mode == "v2_mini" ? 1 : (mode == "v2_blind" ? 2 : 4);
        const int nv = mode == "v2_mini" ? 1 : (mode == "v2_blind" ? 3 : 4);
        for (int ix = 0; ix < nx; ++ix) for (int iv = 0; iv < nv; ++iv) {
            const double x = mode == "v2_blind" ? blind_xs[ix] : train_xs[ix];
            const double v = mode == "v2_blind" ? blind_vs[iv] : train_vs[iv];
            char id[96] = {};
            sprintf_s(id, sizeof(id), "v2_x%.0f_v%.1f", x, v);
            base.push_back({id, x, v, V2_ACCEL_MM_S2, 1,
                            mode == "v2_blind" ? "blind_interpolation" : "stable_identification", 0, 0});
        }
        std::mt19937 rng(session_seed);
        std::shuffle(base.begin(), base.end(), rng);
        for (Condition item : base) {
            item.validation_role = "warmup";
            item.block_id = 0;
            c.push_back(item);
        }
        const int blocks = mode == "v2_mini" ? 2 : (mode == "v2_blind" ? 5 : 10);
        for (int block = 1; block <= blocks; ++block) {
            std::vector<Condition> shuffled = base;
            std::shuffle(shuffled.begin(), shuffled.end(), rng);
            for (Condition& item : shuffled) item.block_id = block;
            c.insert(c.end(), shuffled.begin(), shuffled.end());
        }
    } else if (mode == "d4_mini") {
        const double xs[] = {85.0};
        const double vs[] = {60.0};
        const double as[] = {1000.0};
        addGrid(c, "m3_d4_mini", xs, 1, vs, 1, as, 1, 20, "stable_identification");
    } else if (mode == "d4_mini_v2") {
        const double stable_xs[] = {75.0, 85.0};
        const double stable_vs[] = {30.0, 60.0};
        const double stable_as[] = {500.0, 1000.0};
        const double blind_xs[] = {80.0};
        const double blind_vs[] = {45.0};
        const double blind_as[] = {750.0};
        std::vector<Condition> base;
        for (int i = 0; i < 2; ++i) {
            addGrid(base, "m3_d4_mini_v2_stable", &stable_xs[i], 1, &stable_vs[i], 1, &stable_as[i], 1, 1, "stable_identification");
        }
        addGrid(base, "m3_d4_mini_v2_blind", blind_xs, 1, blind_vs, 1, blind_as, 1, 1, "blind_interpolation");
        std::mt19937 rng(session_seed);
        for (int block = 1; block <= 2; ++block) {
            std::vector<Condition> shuffled = base;
            std::shuffle(shuffled.begin(), shuffled.end(), rng);
            for (Condition& item : shuffled) item.block_id = block;
            c.insert(c.end(), shuffled.begin(), shuffled.end());
        }
    } else if (mode == "d4_controlled") {
        const double xs[] = {75.0, 85.0, 95.0};
        const double vs[] = {30.0, 60.0, 100.0};
        const double as[] = {500.0, 1000.0, 2000.0};
        std::vector<Condition> base;
        addGrid(base, "m3_d4", xs, 3, vs, 3, as, 3, 1, "stable_identification");
        std::mt19937 rng(session_seed);
        for (int block = 1; block <= 10; ++block) {
            std::vector<Condition> shuffled = base;
            std::shuffle(shuffled.begin(), shuffled.end(), rng);
            for (Condition& item : shuffled) item.block_id = block;
            c.insert(c.end(), shuffled.begin(), shuffled.end());
        }
    } else if (mode == "d4_blind") {
        const double xs[] = {80.0, 90.0};
        const double vs[] = {45.0, 75.0};
        const double as[] = {750.0, 1500.0};
        addGrid(c, "m3_d4_blind", xs, 2, vs, 2, as, 2, 10, "blind_interpolation");
    }
    int planned_index = 1;
    for (Condition& item : c) {
        item.planned_trial_index = planned_index;
        planned_index += item.trials;
        if (item.block_id == 0 && !isV2(mode)) item.block_id = 1;
    }
    return c;
}

static bool writePlannedOrder(const std::string& path, const RunContext& ctx, const std::vector<Condition>& conditions) {
    if (fileExists(path)) return false;
    FILE* fp = NULL;
    fopen_s(&fp, path.c_str(), "w");
    if (!fp) return false;
    std::fprintf(fp, "session_id,session_seed,block_id,planned_trial_index,condition_id,target_mm,v_max_mm_s,a_max_mm_s2,hold_time_s,validation_role,v_press_mm_s,v_approach_mm_s,x_switch_mm,condition_repeat_index\n");
    for (const Condition& c : conditions) {
        for (int trial = 0; trial < c.trials; ++trial) {
            std::fprintf(fp, "%s,%u,%d,%d,%s,%.6f,%.6f,%.6f,%.6f,%s,%.6f,%.6f,%.6f,%d\n",
                         ctx.session_id.c_str(), ctx.session_seed, c.block_id, c.planned_trial_index + trial,
                         c.id.c_str(), c.target_mm, c.velocity_mm_s, c.acceleration_mm_s2,
                         isV2(ctx.mode) ? V2_HOLD_S : HOLD_S, c.validation_role.c_str(),
                         isV2(ctx.mode) ? c.velocity_mm_s : 0.0,
                         isV2(ctx.mode) ? V2_APPROACH_MM_S : 0.0,
                         isV2(ctx.mode) ? V2_SWITCH_MM : 0.0,
                         isV2(ctx.mode) ? c.block_id : 0);
        }
    }
    std::fclose(fp);
    return true;
}

static bool openExecutedOrder(RunContext& ctx) {
    if (fileExists(ctx.executed_order_path)) return false;
    fopen_s(&ctx.executed_order, ctx.executed_order_path.c_str(), "w");
    if (!ctx.executed_order) return false;
    std::fprintf(ctx.executed_order,
                 "session_id,session_seed,block_id,planned_trial_index,actual_trial_id,condition_id,validation_role,status,start_time_s,end_time_s\n");
    std::fflush(ctx.executed_order);
    return true;
}

static void writeExecutedOrder(RunContext& ctx, const Condition& c, int planned_index, int trial_id,
                               const char* status, double start_s, double end_s) {
    if (!ctx.executed_order) return;
    std::fprintf(ctx.executed_order, "%s,%u,%d,%d,%d,%s,%s,%s,%.9f,%.9f\n",
                 ctx.session_id.c_str(), ctx.session_seed, c.block_id, planned_index, trial_id,
                 c.id.c_str(), c.validation_role.c_str(), status, start_s, end_s);
    std::fflush(ctx.executed_order);
}

static bool writeSessionConfig(const std::string& path, const RunContext& ctx) {
    if (fileExists(path)) return false;
    FILE* fp = NULL;
    fopen_s(&fp, path.c_str(), "w");
    if (!fp) return false;
    std::fprintf(fp, "{\n");
    std::fprintf(fp, "  \"acquisition_protocol_version\": \"%s\",\n", protocolVersion(ctx.mode));
    std::fprintf(fp, "  \"preprocess_version\": \"%s\",\n", PREPROCESS_VERSION);
    std::fprintf(fp, "  \"training_version\": \"%s\",\n", TRAINING_VERSION);
    std::fprintf(fp, "  \"source_git_commit\": \"%s\",\n", SOURCE_GIT_COMMIT);
    std::fprintf(fp, "  \"executable_build_time\": \"%s\",\n", EXECUTABLE_BUILD_TIME);
    std::fprintf(fp, "  \"session_start_time\": \"%s\",\n", jsonEscape(ctx.session_start_time).c_str());
    std::fprintf(fp, "  \"session_id\": \"%s\",\n", jsonEscape(ctx.session_id).c_str());
    std::fprintf(fp, "  \"session_seed\": %u,\n", ctx.session_seed);
    std::fprintf(fp, "  \"base_seed\": %u,\n", BASE_RANDOM_SEED);
    std::fprintf(fp, "  \"warmup_definition\": \"cycle starts scheduled at 5 + (cycle_index-1)*60 seconds from acquisition t0\",\n");
    std::fprintf(fp, "  \"warmup_cycle_count\": %d,\n", WARMUP_COUNT);
    std::fprintf(fp, "  \"home_zero_mm\": %.3f,\n", HOME_ZERO_MM);
    std::fprintf(fp, "  \"experiment_start_mm\": %.3f,\n", EXPERIMENT_START_MM);
    std::fprintf(fp, "  \"warmup_far_mm\": %.3f,\n", WARMUP_FAR_MM);
    std::fprintf(fp, "  \"v2_switch_mm\": %.3f, \"v2_approach_mm_s\": %.3f, \"v2_accel_mm_s2\": %.3f, \"v2_hold_s\": %.3f\n",
                 V2_SWITCH_MM, V2_APPROACH_MM_S, V2_ACCEL_MM_S2, V2_HOLD_S);
    std::fprintf(fp, "}\n");
    std::fclose(fp);
    return true;
}

static bool writeManifest(const std::string& path, const std::string& csv_path, const char* xml_file,
                          const RunContext& ctx, bool completed, int condition_count) {
    if (fileExists(path)) return false;
    FILE* fp = NULL;
    fopen_s(&fp, path.c_str(), "w");
    if (!fp) return false;
    std::fprintf(fp, "{\n");
    std::fprintf(fp, "  \"package_version\": \"%s\",\n", PACKAGE_VERSION);
    std::fprintf(fp, "  \"acquisition_protocol_version\": \"%s\",\n", protocolVersion(ctx.mode));
    std::fprintf(fp, "  \"preprocess_version\": \"%s\",\n", PREPROCESS_VERSION);
    std::fprintf(fp, "  \"training_version\": \"%s\",\n", TRAINING_VERSION);
    std::fprintf(fp, "  \"source_git_commit\": \"%s\",\n", SOURCE_GIT_COMMIT);
    std::fprintf(fp, "  \"executable_build_time\": \"%s\",\n", EXECUTABLE_BUILD_TIME);
    std::fprintf(fp, "  \"session_start_time\": \"%s\",\n", jsonEscape(ctx.session_start_time).c_str());
    std::fprintf(fp, "  \"experiment\": \"ModelingExperiment3\",\n");
    std::fprintf(fp, "  \"dataset_mode\": \"%s\",\n", jsonEscape(ctx.mode).c_str());
    std::fprintf(fp, "  \"session_id\": \"%s\",\n", jsonEscape(ctx.session_id).c_str());
    std::fprintf(fp, "  \"session_seed\": %u,\n", ctx.session_seed);
    std::fprintf(fp, "  \"base_seed\": %u,\n", BASE_RANDOM_SEED);
    std::fprintf(fp, "  \"csv_path\": \"%s\",\n", jsonEscape(csv_path).c_str());
    std::fprintf(fp, "  \"planned_order_path\": \"%s\",\n", jsonEscape(ctx.planned_order_path).c_str());
    std::fprintf(fp, "  \"executed_order_path\": \"%s\",\n", jsonEscape(ctx.executed_order_path).c_str());
    std::fprintf(fp, "  \"experiment_config_path\": \"%s\",\n", jsonEscape(ctx.session_config_path).c_str());
    std::fprintf(fp, "  \"summary_path\": \"%s\",\n", jsonEscape(ctx.session_summary_path).c_str());
    std::fprintf(fp, "  \"net_xml_file\": \"%s\",\n", jsonEscape(xml_file).c_str());
    std::fprintf(fp, "  \"core\": %d, \"axis\": %d,\n", CORE, AXIS);
    std::fprintf(fp, "  \"home_zero_mm\": %.3f, \"experiment_start_mm\": %.3f, \"warmup_far_mm\": %.3f,\n",
                 HOME_ZERO_MM, EXPERIMENT_START_MM, WARMUP_FAR_MM);
    std::fprintf(fp, "  \"condition_count\": %d,\n", condition_count);
    std::fprintf(fp, "  \"row_count\": %lld,\n", ctx.row_count);
    std::fprintf(fp, "  \"completed\": %s, \"safety_stop\": %s, \"api_failure\": %s,\n",
                 completed ? "true" : "false", ctx.safety_stop ? "true" : "false", ctx.api_failure ? "true" : "false");
    std::fprintf(fp, "  \"warmup_start_to_start_period_s\": %.3f,\n", WARMUP_PERIOD_S);
    std::fprintf(fp, "  \"note\": \"Warm-up cycle starts are scheduled from acquisition t0; blind_interpolation is evaluation-only. current_raw is primary; current_A is provisional.\"\n");
    std::fprintf(fp, "}\n");
    std::fclose(fp);
    return true;
}

static bool writeSessionSummary(const std::string& path, const RunContext& ctx, bool completed) {
    if (fileExists(path)) return false;
    FILE* fp = NULL;
    fopen_s(&fp, path.c_str(), "w");
    if (!fp) return false;
    std::fprintf(fp, "acquisition_protocol_version=%s\n", protocolVersion(ctx.mode));
    std::fprintf(fp, "preprocess_version=%s\n", PREPROCESS_VERSION);
    std::fprintf(fp, "training_version=%s\n", TRAINING_VERSION);
    std::fprintf(fp, "source_git_commit=%s\n", SOURCE_GIT_COMMIT);
    std::fprintf(fp, "executable_build_time=%s\n", EXECUTABLE_BUILD_TIME);
    std::fprintf(fp, "session_start_time=%s\n", ctx.session_start_time.c_str());
    std::fprintf(fp, "session_id=%s\nsession_seed=%u\nrows=%lld\ncompleted=%s\n",
                 ctx.session_id.c_str(), ctx.session_seed, ctx.row_count, completed ? "true" : "false");
    std::fclose(fp);
    return true;
}

static void printUsage(const char* exe) {
    std::printf("Usage: %s [d4_mini|d4_mini_v2|d4_controlled|d4_blind|v2_mini|v2_controlled|v2_blind|--v2-preflight-dry-run|--v2-blind-preflight-dry-run|--preflight-dry-run] [xml_file] [output_root]\n", exe);
}

static const char* modeDescription(const std::string& mode) {
    if (mode == "v2_mini") return "V2 segmented motion preflight: one condition, warmup plus two repeats";
    if (mode == "v2_controlled") return "V2 segmented motion: 16 conditions, warmup plus 10 randomized blocks";
    if (mode == "v2_blind") return "V2 unseen speeds: 6 conditions, warmup plus 5 randomized blocks";
    if (mode == "d4_mini") return "MINI repeatability smoke test: 1 condition, 20 formal trials";
    if (mode == "d4_mini_v2") return "PREFLIGHT MINI: 2 stable + 1 blind condition in each of 2 blocks";
    if (mode == "d4_controlled") return "FULL TRAINING dataset: 27 conditions, 10 randomized blocks";
    if (mode == "d4_blind") return "BLIND interpolation validation dataset: 8 unseen conditions";
    return "UNKNOWN";
}

static std::string promptDatasetMode() {
    std::printf("\nSelect ModelingExperiment3 dataset mode:\n");
    std::printf("  1) d4_mini       - smoke/repeatability test, 1 condition, 20 formal trials\n");
    std::printf("  4) d4_mini_v2    - preflight protocol run; mixed stable/blind roles; not a training file\n");
    std::printf("  2) d4_controlled - FULL TRAINING dataset, 27 conditions, 10 randomized blocks\n");
    std::printf("  3) d4_blind      - blind interpolation validation dataset, 8 unseen conditions\n");
    std::printf("  5) v2_mini / 6) v2_controlled / 7) v2_blind - segmented no-load experiment\n");
    std::printf("Enter mode [default 1=d4_mini]: ");
    char buf[64] = {};
    if (!std::fgets(buf, sizeof(buf), stdin)) return "d4_mini";
    if (buf[0] == '5') return "v2_mini";
    if (buf[0] == '6') return "v2_controlled";
    if (buf[0] == '7') return "v2_blind";
    if (std::strncmp(buf, "v2_controlled", 13) == 0) return "v2_controlled";
    if (std::strncmp(buf, "v2_blind", 8) == 0) return "v2_blind";
    if (std::strncmp(buf, "v2_mini", 7) == 0) return "v2_mini";
    if (buf[0] == '2') return "d4_controlled";
    if (buf[0] == '3') return "d4_blind";
    if (buf[0] == '4') return "d4_mini_v2";
    if (std::strncmp(buf, "d4_controlled", 13) == 0) return "d4_controlled";
    if (std::strncmp(buf, "d4_mini_v2", 10) == 0) return "d4_mini_v2";
    if (std::strncmp(buf, "d4_blind", 8) == 0) return "d4_blind";
    if (std::strncmp(buf, "d4_mini", 7) == 0) return "d4_mini";
    return "d4_mini";
}

int main(int argc, char* argv[]) {
    timeBeginPeriod(1);
    std::string mode = "d4_mini";
    const char* xml_file = DEFAULT_NET_XML_FILE;
    std::string output_root = DEFAULT_OUTPUT_ROOT;
    if (argc >= 2) {
        if (!std::strcmp(argv[1], "-h") || !std::strcmp(argv[1], "--help") || !std::strcmp(argv[1], "/?")) {
            printUsage(argv[0]);
            timeEndPeriod(1);
            return 0;
        }
        mode = argv[1];
    }
    else {
        mode = promptDatasetMode();
    }
    if (argc >= 3) xml_file = argv[2];
    if (argc >= 4) output_root = argv[3];
    const bool software_dry_run = (mode == "--preflight-dry-run" ||
                                   mode == "--v2-preflight-dry-run" ||
                                   mode == "--v2-blind-preflight-dry-run");
    if (mode == "--preflight-dry-run") mode = "d4_mini_v2";
    else if (mode == "--v2-preflight-dry-run") mode = "v2_controlled";
    else if (mode == "--v2-blind-preflight-dry-run") mode = "v2_blind";
    RunContext ctx = {};
    ctx.session_id = makeSessionId();
    if (software_dry_run) {
        char override_id[128] = {};
        const DWORD n = GetEnvironmentVariableA("M3_DRY_RUN_SESSION_ID", override_id, static_cast<DWORD>(sizeof(override_id)));
        if (n > 0 && n < sizeof(override_id)) ctx.session_id = override_id;
    }
    ctx.session_start_time = makeSessionStartTime();
    ctx.mode = mode;
    ctx.session_seed = stableSessionSeed(BASE_RANDOM_SEED, ctx.session_id);
    ctx.active_drift_reference_id = "none";
    ctx.warmup_cycle_index = 0;
    ctx.warmup_scheduled_start_s = std::numeric_limits<double>::quiet_NaN();
    ctx.warmup_actual_start_s = std::numeric_limits<double>::quiet_NaN();
    ctx.warmup_actual_wait_s = std::numeric_limits<double>::quiet_NaN();
    std::vector<Condition> conditions = buildConditions(mode, ctx.session_seed);
    if (conditions.empty()) {
        std::printf("Unknown dataset mode: %s\n", mode.c_str());
        printUsage(argv[0]);
        timeEndPeriod(1);
        return -1;
    }
    std::printf("============================================================\n");
    std::printf("ModelingExperiment3 acquisition startup\n");
    std::printf("Selected dataset mode: %s\n", mode.c_str());
    std::printf("Mode meaning: %s\n", modeDescription(mode));
    std::printf("XML file: %s\n", xml_file);
    std::printf("Output root: %s\n", output_root.c_str());
    if (mode == "d4_controlled") {
        std::printf("FULL TRAINING RUN SELECTED. Warm-up: %d cycles at %.1f s start-to-start before formal randomized blocks.\n", WARMUP_COUNT, WARMUP_PERIOD_S);
    } else if (mode == "d4_blind") {
        std::printf("BLIND VALIDATION RUN SELECTED. Do not use these data for model selection/tuning.\n");
    } else if (mode == "d4_mini_v2") {
        std::printf("PREFLIGHT MINI V2 SELECTED. Mixed stable/blind roles are intentional; this CSV must not be used directly for fitting.\n");
    } else if (mode == "d4_mini") {
        std::printf("MINI RUN SELECTED. This is not the full training dataset.\n");
    }
    std::printf("============================================================\n");

    if (!ensurePackageDirs(output_root)) {
        std::printf("Cannot create output directories under %s\n", output_root.c_str());
        timeEndPeriod(1);
        return -1;
    }

    const std::string csv_path = output_root + "\\data\\raw\\m3_" + mode + "_" + ctx.session_id + ".csv";
    const std::string manifest_path = output_root + "\\manifest\\m3_" + mode + "_" + ctx.session_id + ".json";
    ctx.planned_order_path = output_root + "\\manifest\\planned_order_" + ctx.session_id + ".csv";
    ctx.executed_order_path = output_root + "\\manifest\\executed_order_" + ctx.session_id + ".csv";
    ctx.session_config_path = output_root + "\\manifest\\experiment_config_" + ctx.session_id + ".json";
    ctx.session_summary_path = output_root + "\\manifest\\session_summary_" + ctx.session_id + ".txt";
    if (fileExists(csv_path) || fileExists(manifest_path) || fileExists(ctx.planned_order_path) ||
        fileExists(ctx.executed_order_path) || fileExists(ctx.session_config_path) || fileExists(ctx.session_summary_path)) {
        std::printf("Output collision; refusing to overwrite existing files.\n");
        timeEndPeriod(1);
        return -1;
    }
    if (!writePlannedOrder(ctx.planned_order_path, ctx, conditions) || !writeSessionConfig(ctx.session_config_path, ctx) ||
        !openExecutedOrder(ctx)) {
        std::printf("Cannot create session plan/config/order files.\n");
        timeEndPeriod(1);
        return -1;
    }
    if (software_dry_run) {
        std::fclose(ctx.executed_order);
        ctx.executed_order = NULL;
        writeManifest(manifest_path, csv_path, xml_file, ctx, false, static_cast<int>(conditions.size()));
        writeSessionSummary(ctx.session_summary_path, ctx, false);
        std::printf("Software preflight dry-run complete. No controller connection or motion was attempted.\n");
        std::printf("Manifest: %s\n", manifest_path.c_str());
        timeEndPeriod(1);
        return 0;
    }

    std::printf("Coordinate: mechanical left x=0 mm; operator must place axis at physical x=5 mm before arming.\n");
    if (isV2(mode))
        std::printf("V2 trial: 5 -> 70 mm at 50 mm/s -> target at press speed -> 5 s hold -> 5 mm return. a=200 mm/s^2. No contact load.\n");
    else
        std::printf("Every trial: 5 mm wait -> positive target -> hold -> return 5 mm -> wait. No contact load allowed.\n");
    std::printf("Type NO_CONTACT then Enter to arm: ");
    char confirmation[64] = {};
    if (!std::fgets(confirmation, sizeof(confirmation), stdin) || std::strncmp(confirmation, "NO_CONTACT", 10) != 0) {
        std::printf("Not armed.\n");
        timeEndPeriod(1);
        return 0;
    }

    short ret = 0;
    long net_status = 0;
    const long mask = 1L << (AXIS - 1);
    bool card_opened = false, axis_on = false, ok = true, completed = false;
    int formal_trial_counter = 1;
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
        if (GTN_GetSts(CORE, AXIS, &status, 1, NULL) != 0 || GTN_GetLimitStatus(CORE, AXIS, &lp, &ln) != 0 || isAxisFault(status) || lp || ln) {
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

    {
        double axis_enc = 0.0, raw_enc = 0.0;
        short ra = GTN_GetAxisEncPos(CORE, AXIS, &axis_enc, 1, NULL);
        short rr = GTN_GetEncPos(CORE, AXIS, &raw_enc, 1, NULL);
        if (ra != 0 && rr != 0) { ok = false; goto cleanup; }
        ctx.zero_axis_enc_pulse = axis_enc;
        ctx.zero_raw_enc_pulse = raw_enc;
    }

    fopen_s(&ctx.csv, csv_path.c_str(), "w");
    if (!ctx.csv || !writeCsvHeader(ctx.csv)) { ok = false; goto cleanup; }
    QueryPerformanceFrequency(&ctx.freq);
    QueryPerformanceCounter(&ctx.t0);
    ctx.prev_sample = ctx.t0;
    ctx.unix_t0_s = unixTimeNowSeconds();

    {
        Condition warm = {"m3_warmup_x95_v50", WARMUP_FAR_MM, 50.0,
                          isV2(mode) ? V2_ACCEL_MM_S2 : 1000.0, 1, "warmup", 0, 0};
        std::printf("Warm-up: %d standardized 5 -> 95 -> hold -> 5 cycles at %.1f s start-to-start. Rows are saved with validation_role=warmup and excluded from stable MLP training.\n", WARMUP_COUNT, WARMUP_PERIOD_S);
        double previous_cycle_end_s = 0.0;
        for (int warmup = 1; ok && warmup <= WARMUP_COUNT; ++warmup) {
            const double scheduled_s = WARMUP_INITIAL_WAIT_S + (warmup - 1) * WARMUP_PERIOD_S;
            ctx.warmup_cycle_index = warmup;
            ctx.warmup_scheduled_start_s = scheduled_s;
            ctx.warmup_actual_start_s = std::numeric_limits<double>::quiet_NaN();
            ctx.warmup_actual_wait_s = std::numeric_limits<double>::quiet_NaN();
            if (!sampleUntilRunTime(ctx, warm, warmup, "warmup_scheduled_wait", scheduled_s)) { ok = false; break; }
            LARGE_INTEGER now = {};
            QueryPerformanceCounter(&now);
            ctx.warmup_actual_start_s = qpcSinceStart(now, ctx);
            ctx.warmup_actual_wait_s = ctx.warmup_actual_start_s - previous_cycle_end_s;
            std::printf("  warmup %d/%d scheduled=%.1f s actual=%.3f s actual_wait=%.3f s\n",
                        warmup, WARMUP_COUNT, scheduled_s, ctx.warmup_actual_start_s, ctx.warmup_actual_wait_s);
            if (!runTrial(ctx, warm, warmup)) ok = false;
            QueryPerformanceCounter(&now);
            previous_cycle_end_s = qpcSinceStart(now, ctx);
            std::fflush(ctx.csv);
        }
        ctx.warmup_cycle_index = 0;
        ctx.warmup_scheduled_start_s = std::numeric_limits<double>::quiet_NaN();
        ctx.warmup_actual_start_s = std::numeric_limits<double>::quiet_NaN();
        ctx.warmup_actual_wait_s = std::numeric_limits<double>::quiet_NaN();
    }

    if (!runDriftReference(ctx, 1, "ref_pre")) ok = false;
    formal_trial_counter = 1;
    for (size_t ci = 0; ok && ci < conditions.size(); ++ci) {
        const Condition& cond = conditions[ci];
        std::printf("Condition %zu/%zu: %s target=%.1f v=%.1f a=%.1f trials=%d\n",
                    ci + 1, conditions.size(), cond.id.c_str(), cond.target_mm, cond.velocity_mm_s, cond.acceleration_mm_s2, cond.trials);
        for (int trial = 1; ok && trial <= cond.trials; ++trial) {
            const int formal_trial_id = formal_trial_counter++;
            const int planned_trial_index = cond.planned_trial_index + trial - 1;
            std::printf("  formal_trial_id %d (%d/%d within item)\n", formal_trial_id, trial, cond.trials);
            LARGE_INTEGER trial_start = {}, trial_end = {};
            QueryPerformanceCounter(&trial_start);
            const bool trial_ok = runTrial(ctx, cond, formal_trial_id);
            QueryPerformanceCounter(&trial_end);
            writeExecutedOrder(ctx, cond, planned_trial_index, formal_trial_id, trial_ok ? "completed" : "failed",
                               qpcSinceStart(trial_start, ctx), qpcSinceStart(trial_end, ctx));
            if (!trial_ok) ok = false;
            std::fflush(ctx.csv);
        }
        const size_t reference_interval = (mode == "d4_mini_v2") ? 3u : (isV2(mode) ? 16u : 9u);
        if (ok && ((ci + 1) % reference_interval == 0) && (ci + 1 < conditions.size())) {
            int ref_index = 2 + static_cast<int>((ci + 1) / reference_interval);
            if (!runDriftReference(ctx, ref_index, "ref_mid")) ok = false;
        }
    }
    if (ok) {
        int ref_index = 90;
        if (!runDriftReference(ctx, ref_index, "ref_post")) ok = false;
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
    if (ctx.executed_order) {
        std::fflush(ctx.executed_order);
        std::fclose(ctx.executed_order);
        ctx.executed_order = NULL;
    }
    writeManifest(manifest_path, csv_path, xml_file, ctx, completed, static_cast<int>(conditions.size()));
    writeSessionSummary(ctx.session_summary_path, ctx, completed);
    std::printf("CSV: %s\nManifest: %s\n", csv_path.c_str(), manifest_path.c_str());
    timeEndPeriod(1);
    if (!completed) {
        std::printf("M3 dataset stopped/failed. Keep partial CSV separate and diagnose it.\n");
        return -1;
    }
    std::printf("M3 dataset complete.\n");
    return 0;
}







