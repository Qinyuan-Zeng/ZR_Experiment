#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <conio.h>
#include <windows.h>
#include <mmsystem.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
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

// Spring calibration baseline ONLY. Remove specimen: all 5..95 mm must be free.
// Build this file as the only translation unit. No changes to the training/blind executable.
static const char* PACKAGE_VERSION = "crfc_spring_mapping_acq_v1";
static const char* ACQUISITION_PROTOCOL_VERSION = "d4_acq_v1_1";
static const char* PREPROCESS_VERSION = "m3_preprocess_v1_1";
static const char* TRAINING_VERSION = "m4_train_v1_1";
static const char* SOURCE_GIT_COMMIT = "unavailable";
static const char* EXECUTABLE_BUILD_TIME = __DATE__ " " __TIME__;
static const char* DEFAULT_NET_XML_FILE = "Googol.xml";
static const char* DEFAULT_OUTPUT_ROOT = "E:\\A_ZQY\\Experiment\\CurrentResidualForceCalibration";
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

struct MappingOptions {
    bool loaded = false, pilot = false;
    int current_limit_raw = 0, warmup_limit_raw = 0;
    double force_limit_n = 0.0, contact_mm = 71.5;
    long error_limit_pulse = 10000;
};
static MappingOptions OPTIONS;
static const char* u8text(const char8_t* text) { return reinterpret_cast<const char*>(text); }
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
    bool guard_abort;
    bool stop_monitor;
    double error_over_since;
    const char* abort_reason;
    FILE* events;
    int completed_formal;
    double last_x, last_prf, last_v_prf, last_force;
    short last_current;
    long last_status;
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
static double currentRawToArms(short) {
    return std::numeric_limits<double>::quiet_NaN(); // raw is primary; no measured A calibration.
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
    if (role == "warmup" || role == "condition_warmup") return "warmup";
    if (role == "drift_reference") return "drift_reference";
    return "formal";
}

static bool isV2(const std::string& mode) {
    return true; // All selectable modes use original V2 PRESS/HOLD features.
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
        "limit_positive,limit_negative,enc_source,trap_smooth_time_ms,v_press_mm_s,v_approach_mm_s,x_switch_mm,condition_repeat_index,a_cmd_causal_mm_s2,drift_causal_raw,current_drift_corrected_raw,adc_voltage_v,adc_raw,adc_ret,adc_raw_ret,adc_clock,adc_raw_clock,adc_sample_time_s,force_sensor_n,position_error_pulse,guard_abort,abort_reason,current_limit_raw,force_limit_n,error_limit_pulse\n");
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
    double adc_voltage_v = std::numeric_limits<double>::quiet_NaN();
    short adc_raw = 0;
    unsigned long adc_clock = 0, adc_raw_clock = 0;
    const short adc_ret = GTN_GetAdc(CORE, 1, &adc_voltage_v, 1, &adc_clock);
    const short adc_raw_ret = GTN_GetAdcValue(CORE, 1, &adc_raw, 1, &adc_raw_clock);
    QueryPerformanceCounter(&api_end);
    const double adc_sample_time_s = qpcSinceStart(api_end, ctx);
    const double force_sensor_n = (adc_ret == 0 && adc_raw_ret == 0 && std::isfinite(adc_voltage_v))
        ? 9.81 * adc_voltage_v : std::numeric_limits<double>::quiet_NaN();

    const bool api_failure = r_sts || r_prf_sts || r_prf_pos || r_prf_vel || (r_axis_pos && r_raw_pos) ||
                             r_axis_vel || r_prf_acc || r_error || r_limit || r_current || adc_ret || adc_raw_ret || !std::isfinite(adc_voltage_v);
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

    ctx.last_x = x_act; ctx.last_prf = x_prf; ctx.last_v_prf = v_prf;
    ctx.last_force = force_sensor_n; ctx.last_current = current_raw; ctx.last_status = axis_status;
    const bool warm = cond.validation_role == "warmup";
    const int current_cap = warm ? OPTIONS.warmup_limit_raw : OPTIONS.current_limit_raw;
    const char* cause = nullptr;
    if (api_failure) cause = "api_failure";
    else if (isAxisFault(axis_status) || lim_pos || lim_neg) cause = "fault_or_limit";
    else if (!std::isfinite(x_act) || x_act < GUARD_LOW_MM || x_act > GUARD_HIGH_MM) cause = "position_guard";
    else if (std::abs(static_cast<int>(current_raw)) >= current_cap) cause = "current_limit";
    else if (std::fabs(force_sensor_n) >= OPTIONS.force_limit_n) cause = "force_limit";
    else if (OPTIONS.loaded && (x_act > 85.05 || x_prf > 85.05)) cause = "loaded_max_position";
    else if (OPTIONS.loaded && (warm || std::strcmp(phase, "FAST_APPROACH") == 0) && std::fabs(force_sensor_n) >= 1.0) cause = "unexpected_fast_contact";
    else if (std::fabs((x_prf-x_act)*PULSE_PER_MM) > OPTIONS.error_limit_pulse) {
        if (ctx.error_over_since < 0) ctx.error_over_since = run_time_s;
        if (run_time_s-ctx.error_over_since >= 0.005) cause = "following_error";
    } else ctx.error_over_since = -1;
    if (cause && !ctx.stop_monitor && !ctx.guard_abort) {
        ctx.guard_abort = true; ctx.abort_reason = cause;
        if (ctx.events) {
            std::fprintf(ctx.events, "%d,%.9f,%s,%s,%.9f,%.9f,%d,%.9f\n", trial_id, run_time_s, phase, cause, x_act, x_prf, current_raw, force_sensor_n);
            std::fflush(ctx.events);
        }
        std::printf("GUARD: %s phase=%s actual=%.6f planned=%.6f I=%d F=%.6f N\n", cause, phase, x_act, x_prf, current_raw, force_sensor_n);
    }
    const double api_us = (qpcSeconds(api_end, ctx.freq) - qpcSeconds(api_start, ctx.freq)) * 1e6;
    std::fprintf(ctx.csv,
        "%.9f,%.9f,%.9f,%s,%s,%d,%s,%s,%.9f,%s,%s,%s,%s,"
        "%s,%s,%s,%s,%s,%s,%u,%d,%d,%d,%.9f,%.9f,%.9f,"
        "%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,"
        "%d,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,"
        "%s,%s,%.9f,%.9f,0x%08lX,%.9f,%s,%.9f,%.9f,%.9f,%.9f,"
        "%d,%d,%lld,%ld,%.9f,%.3f,0x%08lX,%d,%d,%s,%d,%.9f,%.9f,%.9f,%d,%.9f,%.9f,%.9f",
        ctx.unix_t0_s + run_time_s, run_time_s, run_time_s, ctx.session_id.c_str(), ctx.mode.c_str(), trial_id,
        cond.id.c_str(), phase, phase_time_s, reference_id, ctx.active_drift_reference_id.c_str(), PACKAGE_VERSION, cond.validation_role.c_str(),
        recordTypeForRole(cond.validation_role), (api_failure || ctx.safety_stop || ctx.guard_abort) ? "invalid" : "ok",
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
    std::fprintf(ctx.csv, ",%.9f,%d,%d,%d,%lu,%lu,%.9f,%.9f,%.3f",
        adc_voltage_v, adc_raw, adc_ret, adc_raw_ret, adc_clock, adc_raw_clock,
        adc_sample_time_s, force_sensor_n, axis_error);
    std::fprintf(ctx.csv, ",%d,%s,%d,%.6f,%ld\n", ctx.guard_abort ? 1 : 0,
        ctx.abort_reason ? ctx.abort_reason : "none", current_cap, OPTIONS.force_limit_n, OPTIONS.error_limit_pulse);
    if (std::ferror(ctx.csv)) {
        ctx.api_failure = true;
        ctx.safety_stop = true;
    }
    ++ctx.row_count;
    return !ctx.safety_stop && (!ctx.guard_abort || ctx.stop_monitor);
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
        if (!isMoving(axis_status)) {
            if (std::fabs(ctx.last_prf-target_mm)>0.005) {ctx.guard_abort=true;ctx.abort_reason="planner_not_at_target";return false;}
            return true;
        }
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
                                EXPERIMENT_START_MM, 0.0, "none", OPTIONS.loaded ? "spring_installed" : "no_contact_confirmed")) return false;
        QueryPerformanceCounter(&now);
        if (qpcSinceStart(now, ctx) >= target_run_time_s) return true;
        if (IDLE_SAMPLE_SLEEP_MS > 0) Sleep(IDLE_SAMPLE_SLEEP_MS);
    }
}

static bool runTrial(RunContext& ctx, const Condition& cond, int trial_id) {
    if (!configureTrap(cond)) return false;
    if (!sampleForDuration(ctx, cond, trial_id, "wait_start_pre", START_WAIT_S, EXPERIMENT_START_MM, 0.0, "none", OPTIONS.loaded ? "spring_installed" : "no_contact_confirmed")) return false;
    ctx.error_over_since=-1;
    if (std::fabs(ctx.last_prf-5.0)>0.001 || std::fabs(ctx.last_x-5.0)>0.15 || isMoving(ctx.last_status)) {
        ctx.guard_abort=true;ctx.abort_reason="start_position_not_ready";return false;
    }
    if (isV2(ctx.mode)) {
        if (!moveAndSample(ctx, cond, trial_id, "FAST_APPROACH", V2_SWITCH_MM, V2_APPROACH_MM_S, "none", OPTIONS.loaded ? "spring_installed" : "no_contact_confirmed")) return false;
        if (!moveAndSample(ctx, cond, trial_id, "PRESS", cond.target_mm, cond.velocity_mm_s, "none", OPTIONS.loaded ? "spring_installed" : "no_contact_confirmed")) return false;
        if (!sampleForDuration(ctx, cond, trial_id, "HOLD", V2_HOLD_S, cond.target_mm, 0.0, "none", OPTIONS.loaded ? "spring_installed" : "no_contact_confirmed")) return false;
        if (!moveAndSample(ctx, cond, trial_id, "RETURN", EXPERIMENT_START_MM, -V2_APPROACH_MM_S, "none", OPTIONS.loaded ? "spring_installed" : "no_contact_confirmed")) return false;
        return sampleForDuration(ctx, cond, trial_id, "wait_start_post", RETURN_WAIT_S, EXPERIMENT_START_MM, 0.0, "none", OPTIONS.loaded ? "spring_installed" : "no_contact_confirmed");
    }
    if (!moveAndSample(ctx, cond, trial_id, "move_forward", cond.target_mm, cond.velocity_mm_s, "none", OPTIONS.loaded ? "spring_installed" : "no_contact_confirmed")) return false;
    if (!sampleForDuration(ctx, cond, trial_id, "hold_at_target", HOLD_S, cond.target_mm, 0.0, "none", OPTIONS.loaded ? "spring_installed" : "no_contact_confirmed")) return false;
    if (!moveAndSample(ctx, cond, trial_id, "move_return", EXPERIMENT_START_MM, -cond.velocity_mm_s, "none", OPTIONS.loaded ? "spring_installed" : "no_contact_confirmed")) return false;
    return sampleForDuration(ctx, cond, trial_id, "wait_start_post", RETURN_WAIT_S, EXPERIMENT_START_MM, 0.0, "none", OPTIONS.loaded ? "spring_installed" : "no_contact_confirmed");
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

static std::vector<Condition> buildConditions(const std::string&, unsigned int seed) {
    std::vector<Condition> result;
    if (OPTIONS.pilot) {
        result.push_back({"spring_pilot_x75_v5",75.0,5.0,200.0,1,"spring_pilot",1,1});
        return result;
    }
    std::vector<Condition> base;
    for (double x : {80.2,84.5}) {
        char id[80] = {}; sprintf_s(id,sizeof(id),"spring_mapping_x%.1f_v5",x);
        base.push_back({id,x,5.0,200.0,1,OPTIONS.loaded?"spring_calibration":"calibration_no_load",0,0});
    }
    if (!OPTIONS.loaded) {
        for (const auto& co : base) { auto w=co; w.validation_role="condition_warmup";result.push_back(w); }
        auto w=base[0];w.validation_role="condition_warmup";result.push_back(w);
    }
    std::mt19937 rng(seed);
    for (int block=1;block<=3;++block) {
        auto shuffled=base;std::shuffle(shuffled.begin(),shuffled.end(),rng);
        for (auto co:shuffled) {co.block_id=block;result.push_back(co);}
    }
    int i=1;for(auto& co:result)co.planned_trial_index=i++;
    return result;
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
    std::fprintf(fp, "  \"formal_trial_count_planned\": %d, \"condition_warmup_count_planned\": %d,\n", OPTIONS.pilot?1:6, OPTIONS.loaded?0:3);
    std::fprintf(fp, "  \"targets_mm\": %s, \"press_speed_mm_s\": 5.0, \"hold_s\": 5.0,\n", OPTIONS.pilot?"[75.0]":"[80.2,84.5]");
    std::fprintf(fp, "  \"specimen_installed\": %s, \"adc_required\": true, \"adc_channel\": 1,\n", OPTIONS.loaded?"true":"false");
    std::fprintf(fp, "  \"current_limit_raw\": %d, \"warmup_limit_raw\": %d, \"force_limit_n\": %.9f, \"error_limit_pulse\": %ld,\n",OPTIONS.current_limit_raw,OPTIONS.warmup_limit_raw,OPTIONS.force_limit_n,OPTIONS.error_limit_pulse);
    std::fprintf(fp, "  \"contact_program_mm\": %.9f, \"guard_abort\": %s, \"abort_reason\": \"%s\", \"completed_formal_trials\": %d,\n",OPTIONS.contact_mm,ctx.guard_abort?"true":"false",ctx.abort_reason?ctx.abort_reason:"none",ctx.completed_formal);
    std::fprintf(fp, "  \"loaded_warmup_cycles\": %d, \"loaded_warmup_far_mm\": 10.0, \"unload_method\": \"original_RETURN_50mm_s_with_live_guards\",\n",OPTIONS.loaded?3:0);
    std::fprintf(fp, "  \"sensor_force_coefficient_n_per_v\": 9.81, \"sensor_force_zero_v\": 0.0,\n");

    std::fprintf(fp, "  \"warmup_definition\": \"%s\",\n",OPTIONS.loaded?"3 contact-free 5..10 mm cycles, no long-period scheduling":"6 starts scheduled at 5 + (index-1)*60 s");
    std::fprintf(fp, "  \"warmup_cycle_count\": %d,\n", OPTIONS.loaded?3:WARMUP_COUNT);
    std::fprintf(fp, "  \"fixed_start_command_pulses_after_homing\": 4000000, \"fixed_start_program_mm\": 5.0, \"pulse_per_mm\": 10000.0,\n");
    std::fprintf(fp, "  \"home_zero_mm\": %.3f,\n", HOME_ZERO_MM);
    std::fprintf(fp, "  \"experiment_start_mm\": %.3f,\n", EXPERIMENT_START_MM);
    std::fprintf(fp, "  \"warmup_far_mm\": %.3f,\n", (OPTIONS.loaded?10.0:WARMUP_FAR_MM));
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
    std::fprintf(fp, "  \"experiment\": \"CurrentResidualForceCalibration\",\n");
    std::fprintf(fp, "  \"dataset_mode\": \"%s\",\n", jsonEscape(ctx.mode).c_str());
    std::fprintf(fp, "  \"session_id\": \"%s\",\n", jsonEscape(ctx.session_id).c_str());
    std::fprintf(fp, "  \"session_seed\": %u,\n", ctx.session_seed);
    std::fprintf(fp, "  \"base_seed\": %u,\n", BASE_RANDOM_SEED);
    std::fprintf(fp, "  \"formal_trial_count_planned\": %d, \"condition_warmup_count_planned\": %d,\n", OPTIONS.pilot?1:6, OPTIONS.loaded?0:3);
    std::fprintf(fp, "  \"targets_mm\": %s, \"press_speed_mm_s\": 5.0, \"hold_s\": 5.0,\n", OPTIONS.pilot?"[75.0]":"[80.2,84.5]");
    std::fprintf(fp, "  \"specimen_installed\": %s, \"adc_required\": true, \"adc_channel\": 1,\n", OPTIONS.loaded?"true":"false");
    std::fprintf(fp, "  \"current_limit_raw\": %d, \"warmup_limit_raw\": %d, \"force_limit_n\": %.9f, \"error_limit_pulse\": %ld,\n",OPTIONS.current_limit_raw,OPTIONS.warmup_limit_raw,OPTIONS.force_limit_n,OPTIONS.error_limit_pulse);
    std::fprintf(fp, "  \"contact_program_mm\": %.9f, \"guard_abort\": %s, \"abort_reason\": \"%s\", \"completed_formal_trials\": %d,\n",OPTIONS.contact_mm,ctx.guard_abort?"true":"false",ctx.abort_reason?ctx.abort_reason:"none",ctx.completed_formal);
    std::fprintf(fp, "  \"loaded_warmup_cycles\": %d, \"loaded_warmup_far_mm\": 10.0, \"unload_method\": \"original_RETURN_50mm_s_with_live_guards\",\n",OPTIONS.loaded?3:0);
    std::fprintf(fp, "  \"sensor_force_coefficient_n_per_v\": 9.81, \"sensor_force_zero_v\": 0.0,\n");

    std::fprintf(fp, "  \"csv_path\": \"%s\",\n", jsonEscape(csv_path).c_str());
    std::fprintf(fp, "  \"planned_order_path\": \"%s\",\n", jsonEscape(ctx.planned_order_path).c_str());
    std::fprintf(fp, "  \"executed_order_path\": \"%s\",\n", jsonEscape(ctx.executed_order_path).c_str());
    std::fprintf(fp, "  \"experiment_config_path\": \"%s\",\n", jsonEscape(ctx.session_config_path).c_str());
    std::fprintf(fp, "  \"summary_path\": \"%s\",\n", jsonEscape(ctx.session_summary_path).c_str());
    std::fprintf(fp, "  \"net_xml_file\": \"%s\",\n", jsonEscape(xml_file).c_str());
    std::fprintf(fp, "  \"core\": %d, \"axis\": %d,\n", CORE, AXIS);
    std::fprintf(fp, "  \"home_zero_mm\": %.3f, \"experiment_start_mm\": %.3f, \"warmup_far_mm\": %.3f,\n",
                 HOME_ZERO_MM, EXPERIMENT_START_MM, (OPTIONS.loaded?10.0:WARMUP_FAR_MM));
    std::fprintf(fp, "  \"condition_count\": %d,\n", condition_count);
    std::fprintf(fp, "  \"row_count\": %lld,\n", ctx.row_count);
    std::fprintf(fp, "  \"completed\": %s, \"safety_stop\": %s, \"api_failure\": %s,\n",
                 completed ? "true" : "false", ctx.safety_stop ? "true" : "false", ctx.api_failure ? "true" : "false");
    std::fprintf(fp, "  \"warmup_start_to_start_period_s\": %.3f,\n", WARMUP_PERIOD_S);
    std::fprintf(fp, "  \"note\": \"No-load uses original long warmup; loaded only short warmup to10 mm. MLP1 remains frozen and is evaluated offline. Software guards are not drive hardware limits.\"\n");
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
    std::printf("Usage: %s [--dry-run] [xml_file] [output_root]\n", exe);
    std::printf("No-load spring baseline ONLY: 80.2/84.5/88.9 mm at 5 mm/s; 3 condition warmups + 9 formal trials; HOLD 5 s; ADC channel 1 required.\n");
}

static const char* modeDescription(const std::string&) {
    return "Original V2 formal protocol: 3 endpoints, 3 condition warmups + 9 formal trials; six original long warmups; required force ADC";
}


static bool inputNumber(const char8_t* prompt, double& value, double lo, double hi, bool allowBlank=false) {
    for (;;) {
        std::printf("%s",u8text(prompt));char line[128]={};
        if(!std::fgets(line,sizeof(line),stdin))return false;
        line[std::strcspn(line,"\r\n")]=0;
        if(!line[0]) {if(allowBlank)return true;std::printf("Required; blank does not arm motion.\n");continue;}
        char* end=nullptr;const double n=std::strtod(line,&end);
        if(end && !*end && std::isfinite(n) && n>=lo && n<=hi){value=n;return true;}
        std::printf("Invalid value; allowed %.3f .. %.3f.\n",lo,hi);
    }
}
static bool token(const char8_t* prompt,const char* expected) {
    std::printf("%s [%s]: ",u8text(prompt),expected);char line[100]={};
    if(!std::fgets(line,sizeof(line),stdin))return false;
    line[std::strcspn(line,"\r\n")]=0;return !std::strcmp(line,expected);
}
static bool stopAndRecord(RunContext& ctx,const Condition& co,int tid) {
    if(GTN_Stop(CORE,1L<<(AXIS-1),0)!=0){ctx.api_failure=true;return false;}
    ctx.stop_monitor=true;LARGE_INTEGER start={};QueryPerformanceCounter(&start);
    const double st=qpcSinceStart(start,ctx);double stable=-1;bool stopped=false;
    for(long index=0;;++index) {
        if(!readAndWriteSample(ctx,co,tid,"ABORT_STOP",index,st,ctx.last_prf,0,"none",OPTIONS.loaded?"spring_installed":"no_contact_confirmed"))break;
        if(!isMoving(ctx.last_status)&&std::fabs(ctx.last_v_prf)<0.001) {
            LARGE_INTEGER now={};QueryPerformanceCounter(&now);double t=qpcSinceStart(now,ctx);
            if(stable<0)stable=t;
            if(t-stable>=0.1){stopped=true;break;}
        } else stable=-1;
        LARGE_INTEGER now={};QueryPerformanceCounter(&now);if(qpcSinceStart(now,ctx)-st>2.0)break;
        Sleep(1);
    }
    ctx.stop_monitor=false;return stopped;
}
int main(int argc,char** argv) {
    SetConsoleOutputCP(CP_UTF8);SetConsoleCP(CP_UTF8);timeBeginPeriod(1);
    bool dry=argc>1&&!std::strcmp(argv[1],"--dry-run");
    std::string output=DEFAULT_OUTPUT_ROOT;
    if(dry && argc>2)output=argv[2];
    int mode=3;
    if(!dry) {
        std::printf("%s",u8text(u8"弹簧残差—力关系采集（不在线估力、不重训MLP-1）\n1. 空载对照：移除弹簧；6次原长预热到95 mm，3次条件预跑，6条正式\n2. 带载浅压摸底：75 mm / 5 mm/s / HOLD 5 s，1次\n3. 带载正式：80.2、84.5 mm / 5 mm/s / HOLD 5 s，各3次\n4. 仅生成正式带载计划（不连接、不运动）\n0. 退出\n请选择0/1/2/3/4： "));
        char choice[32]={};if(!std::fgets(choice,sizeof(choice),stdin)){timeEndPeriod(1);return 0;}
        choice[std::strcspn(choice,"\r\n")]=0;
        if(std::strlen(choice)!=1 || choice[0]<'0'||choice[0]>'4'){timeEndPeriod(1);return 1;}
        mode=choice[0]-'0';if(mode==0){timeEndPeriod(1);return 0;}if(mode==4){dry=true;mode=3;}
    }
    OPTIONS.loaded=mode!=1;OPTIONS.pilot=mode==2;
    if(!dry) {
        double value=0;
        if(!inputNumber(u8"正式采集绝对电流上限raw（必须填写；不是驱动器限流）： ",value,1,32767))return 1;
        if(value!=std::floor(value))return 1;OPTIONS.current_limit_raw=static_cast<int>(value);
        if(!inputNumber(u8"预热绝对电流上限raw（必须填写，启动峰值可能高于PRESS）： ",value,1,32767))return 1;
        if(value!=std::floor(value))return 1;OPTIONS.warmup_limit_raw=static_cast<int>(value);
        if(!inputNumber(u8"绝对传感器力上限N（必须填写；F=9.81×V）： ",OPTIONS.force_limit_n,0.1,10000))return 1;
        value=10000;if(!inputNumber(u8"跟随误差上限pulse [10000，回车保留；持续5ms触发]： ",value,3000,10000,true))return 1;
        if(value!=std::floor(value))return 1;OPTIONS.error_limit_pulse=static_cast<long>(value);
        if(OPTIONS.loaded && !inputNumber(u8"确认接触程序位置mm [71.5，回车保留；必须在70 mm之后]： ",OPTIONS.contact_mm,70.5,72.0,true))return 1;
        if(mode==3 && !token(u8"确认75 mm浅压摸底已完成并审查力、电流、安装状态", "PILOT_REVIEWED"))return 0;
        if(!token(u8"确认已独立回零，并将同一轴沿压入方向前进4,000,000脉冲至固定起点", "HOME_PLUS_4000000"))return 0;
        if(mode==1) {if(!token(u8"弹簧已移除，5～95 mm全程无接触", "NO_CONTACT"))return 0;}
        else if(!token(u8"弹簧接触位置已核查，5～10 mm短预热无接触，保护上限已确认", "SPRING_LIMITS_REVIEWED"))return 0;
    }
    RunContext ctx={};ctx.error_over_since=-1;ctx.abort_reason="none";
    ctx.mode=OPTIONS.loaded?(OPTIONS.pilot?"spring_pilot_loaded":"spring_mapping_loaded"):"spring_mapping_no_load";
    ctx.session_id=makeSessionId();ctx.session_start_time=makeSessionStartTime();ctx.session_seed=stableSessionSeed(BASE_RANDOM_SEED,ctx.session_id);
    ctx.active_drift_reference_id="none";
    ctx.warmup_scheduled_start_s=ctx.warmup_actual_start_s=ctx.warmup_actual_wait_s=std::numeric_limits<double>::quiet_NaN();
    const auto conditions=buildConditions(ctx.mode,ctx.session_seed);
    if(!ensurePackageDirs(output)){timeEndPeriod(1);return 1;}
    const std::string prefix=output+"\\manifest\\crfc_"+ctx.mode+"_"+ctx.session_id;
    const std::string csv=output+"\\data\\raw\\crfc_"+ctx.mode+"_"+ctx.session_id+".csv";
    const std::string manifest=prefix+".json",events=prefix+"_events.csv";
    ctx.planned_order_path=prefix+"_planned.csv";ctx.executed_order_path=prefix+"_executed.csv";
    ctx.session_config_path=prefix+"_config.json";ctx.session_summary_path=prefix+"_summary.txt";
    for(const auto& p:{csv,manifest,events,ctx.planned_order_path,ctx.executed_order_path,ctx.session_config_path,ctx.session_summary_path})
        if(fileExists(p)){std::printf("Output collision; not overwriting.\n");timeEndPeriod(1);return 1;}
    if(!writePlannedOrder(ctx.planned_order_path,ctx,conditions)||!writeSessionConfig(ctx.session_config_path,ctx)||!openExecutedOrder(ctx)){timeEndPeriod(1);return 1;}
    if(dry) {
        std::fclose(ctx.executed_order);ctx.executed_order=nullptr;
        writeManifest(manifest,csv,DEFAULT_NET_XML_FILE,ctx,false,static_cast<int>(conditions.size()));
        std::printf("DRY RUN: six formal trials, 80.2/84.5 at5, HOLD5; three short warmups to10; no95 movement. No controller connection.\nPlan: %s\n",ctx.planned_order_path.c_str());timeEndPeriod(1);return 0;
    }
    QueryPerformanceFrequency(&ctx.freq);QueryPerformanceCounter(&ctx.t0);ctx.unix_t0_s=unixTimeNowSeconds();
    bool card=false,enabled=false,ok=true,complete=false,keep_enabled=false;
    short ret=0;long net=0;const long mask=1L<<(AXIS-1);
    fopen_s(&ctx.csv,csv.c_str(),"w");fopen_s(&ctx.events,events.c_str(),"w");
    if(!ctx.csv||!ctx.events||!writeCsvHeader(ctx.csv)){ok=false;goto cleanup;}
    std::fprintf(ctx.events,"trial_id,time_s,phase,event,x_act_mm,x_prf_mm,current_raw,force_sensor_n\n");
    ret=GTN_OpenCard();if(ret){ctx.api_failure=true;ok=false;goto cleanup;}card=true;
    ret=GTN_NetInit(NET_INIT_MODE_XML_STRICT,const_cast<char*>(DEFAULT_NET_XML_FILE),60,&net);
    if(ret){std::printf("NetInit failed ret=%d status=%ld\n",ret,net);ctx.api_failure=true;ok=false;goto cleanup;}
    {
        double v=0,pv=0;short raw=0,lp=0,ln=0;long status=0;
        if(GTN_GetAdc(CORE,1,&v,1,NULL)||GTN_GetAdcValue(CORE,1,&raw,1,NULL)||!std::isfinite(v)||
           GTN_GetSts(CORE,AXIS,&status,1,NULL)||GTN_GetPrfVel(CORE,AXIS,&pv,1,NULL)||GTN_GetLimitStatus(CORE,AXIS,&lp,&ln)||
           isAxisFault(status)||isMoving(status)||std::fabs(pv)>0.001||lp||ln||std::fabs(9.81*v)>=1.0) {
            std::printf("Initial ADC/stopped/limit/zero-force check failed; no AxisOn/zero/move.\n");ctx.api_failure=true;ok=false;goto cleanup;
        }
    }
    if(GTN_ClrSts(CORE,AXIS,1)){ctx.api_failure=true;ok=false;goto cleanup;}
    if(GTN_AxisOn(CORE,AXIS)){ctx.api_failure=true;ok=false;goto cleanup;}enabled=true;
    { // Startup electrical guard before local coordinates are reset.
        const ULONGLONG begin=GetTickCount64();
        do {
            short cur=0;double v=0;
            if(GTN_GetAtlTorque(CORE,AXIS,&cur)||GTN_GetAdc(CORE,1,&v,1,NULL)||!std::isfinite(v)){ctx.api_failure=true;ok=false;goto cleanup;}
            if(std::abs(static_cast<int>(cur))>=OPTIONS.warmup_limit_raw || std::fabs(9.81*v)>=OPTIONS.force_limit_n) {
                ctx.guard_abort=true;ctx.abort_reason="enable_startup_guard";std::fprintf(ctx.events,"0,0,ENABLE,%s,nan,nan,%d,%.9f\n",ctx.abort_reason,cur,9.81*v);ok=false;goto cleanup;
            }
            Sleep(1);
        } while(GetTickCount64()-begin<500);
    }
    if(GTN_ZeroPos(CORE,AXIS,1)||GTN_SetPrfPos(CORE,AXIS,0)||GTN_SetSoftLimitMode(CORE,AXIS,LIMIT_MODE_SOFT)||
       GTN_SetSoftLimit(CORE,AXIS,physicalMmToPulse(OPTIONS.loaded?85.0:99.0),physicalMmToPulse(1.0))||GTN_PrfTrap(CORE,AXIS)) {
        ctx.api_failure=true;ok=false;goto cleanup;
    }
    {
        short a=GTN_GetAxisEncPos(CORE,AXIS,&ctx.zero_axis_enc_pulse,1,NULL),b=GTN_GetEncPos(CORE,AXIS,&ctx.zero_raw_enc_pulse,1,NULL);
        if(a&&b){ctx.api_failure=true;ok=false;goto cleanup;}
    }
    QueryPerformanceCounter(&ctx.t0);ctx.unix_t0_s=unixTimeNowSeconds();
    {
        Condition warm={"platform_warmup",OPTIONS.loaded?10.0:95.0,50.0,200.0,1,"warmup",0,0};
        const int count=OPTIONS.loaded?3:6;
        for(int i=1;ok&&i<=count;++i) {
            ctx.warmup_cycle_index=i;
            if(!OPTIONS.loaded) {
                ctx.warmup_scheduled_start_s=5+(i-1)*60;
                if(!sampleUntilRunTime(ctx,warm,i,"warmup_scheduled_wait",ctx.warmup_scheduled_start_s)){ok=false;break;}
            }
            LARGE_INTEGER now={};QueryPerformanceCounter(&now);ctx.warmup_actual_start_s=qpcSinceStart(now,ctx);
            if(OPTIONS.loaded) {
                if(!configureTrap(warm)||!sampleForDuration(ctx,warm,i,"warmup_wait",1.5,5,0,"none","no_contact_short_warmup")||
                   !moveAndSample(ctx,warm,i,"warmup_forward",10,50,"none","no_contact_short_warmup")||
                   !sampleForDuration(ctx,warm,i,"warmup_hold",1,10,0,"none","no_contact_short_warmup")||
                   !moveAndSample(ctx,warm,i,"warmup_return",5,-50,"none","no_contact_short_warmup")||
                   !sampleForDuration(ctx,warm,i,"warmup_wait_post",1.5,5,0,"none","no_contact_short_warmup"))ok=false;
            } else if(!runTrial(ctx,warm,i))ok=false;
            std::fflush(ctx.csv);std::printf("Warmup %d/%d %s\n",i,count,ok?"complete":"FAILED");
        }
        ctx.warmup_cycle_index=0;
        ctx.warmup_scheduled_start_s=ctx.warmup_actual_start_s=ctx.warmup_actual_wait_s=std::numeric_limits<double>::quiet_NaN();
    }
    for(size_t i=0;ok&&i<conditions.size();++i) {
        const auto& co=conditions[i];LARGE_INTEGER a={},b={};QueryPerformanceCounter(&a);
        std::printf("Trial %zu/%zu role=%s target%.1f PRESS5 HOLD5\n",i+1,conditions.size(),co.validation_role.c_str(),co.target_mm);
        const bool done=runTrial(ctx,co,static_cast<int>(i+1));QueryPerformanceCounter(&b);
        writeExecutedOrder(ctx,co,co.planned_trial_index,static_cast<int>(i+1),done?"completed":"failed",qpcSinceStart(a,ctx),qpcSinceStart(b,ctx));
        if(done && co.validation_role!="condition_warmup")++ctx.completed_formal;
        if(!done){ok=false;if(!ctx.abort_reason || !std::strcmp(ctx.abort_reason,"none"))ctx.abort_reason="motion_or_user_abort";
            if(!ctx.api_failure && !ctx.safety_stop)keep_enabled=stopAndRecord(ctx,co,static_cast<int>(i+1));
        }
        std::fflush(ctx.csv);
    }
    complete=ok&&!ctx.guard_abort&&!ctx.safety_stop&&!ctx.api_failure;
cleanup:
    if(card) {
        const short stop_ret=GTN_Stop(CORE,mask,(ctx.api_failure||ctx.safety_stop)?mask:0);
        if(stop_ret){ctx.api_failure=true;complete=false;keep_enabled=false;}
        if(enabled && !keep_enabled)GTN_AxisOff(CORE,AXIS);
        GTN_Close();
    }
    if(ctx.csv){std::fflush(ctx.csv);std::fclose(ctx.csv);}
    if(ctx.events){std::fflush(ctx.events);std::fclose(ctx.events);}
    if(ctx.executed_order)std::fclose(ctx.executed_order);
    writeManifest(manifest,csv,DEFAULT_NET_XML_FILE,ctx,complete,static_cast<int>(conditions.size()));
    writeSessionSummary(ctx.session_summary_path,ctx,complete);
    std::printf("CSV: %s\nManifest: %s\nCompleted formal: %d/%d\n",csv.c_str(),manifest.c_str(),ctx.completed_formal,OPTIONS.pilot?1:6);
    if(keep_enabled)std::printf("CONTROLLED STOP confirmed; AxisOff was NOT sent. Residual spring load may remain. Manually unload with the device controls; no automatic retry/return.\n");
    if(!complete)std::printf("STOPPED reason=%s api_failure=%d safety_stop=%d guard_abort=%d. Partial data are not complete calibration trials.\n",ctx.abort_reason,ctx.api_failure,ctx.safety_stop,ctx.guard_abort);
    timeEndPeriod(1);return complete?0:1;
}
