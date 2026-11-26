// Horizontal motor only. No nonzero force command and no trajectory command.
// The test condition intentionally triggers at rest; it is NOT contact detection.
#include <cstdio>
#include <cmath>
#include <cwchar>
#include <conio.h>
#include "gvn_force_api_ext.h"

namespace {
const short CORE = 2, AXIS = 1, INDEX = 1;
const double PULSE_PER_MM = 10000.0;
const double MAX_TRAVEL_PULSE = 2000.0; // 0.2 mm from initial encoder reading
const double MAX_VELOCITY_PULSE_MS = 50.0; // 5 mm/s
const double MAX_POSITION_ERROR_PULSE = 1000.0; // 0.1 mm
const int MAX_CURRENT_RAW = 400; // Software trip only; NOT a drive current clamp.
const DWORD POLL_MS = 5, ENABLE_WAIT_MS = 200, SWITCH_TIMEOUT_MS = 500;
FILE* logFile = nullptr;
double origin = 0;
bool originValid = false, enableAttempted = false, switchTouched = false;
TSwitchLoopParameter originalSwitch = {};
volatile LONG cancelRequested = 0;

BOOL WINAPI consoleHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        InterlockedExchange(&cancelRequested, 1);
        return TRUE;
    }
    return FALSE;
}

#define LOG(...) do { std::printf(__VA_ARGS__); if (logFile) { \
    std::fprintf(logFile, __VA_ARGS__); std::fflush(logFile); } } while (0)

bool ok(const char* name, short rc) {
    LOG("%s: rc=%d\n", name, rc);
    return rc == 0;
}

struct Sample {
    long sts = 0;
    short mode = -1, current = 0;
    double encoder = 0, planned = 0, velocity = 0;
};

bool sample(Sample& s, bool requireEnabled, const char* phase) {
    if (InterlockedCompareExchange(&cancelRequested, 0, 0)) {
        LOG("ABORT: console cancellation requested.\n"); return false;
    }
    // Evaluate each call independently so every return code is captured.
    const short r1 = GTN_GetSts(CORE, AXIS, &s.sts);
    const short r2 = GTN_GetControlLoopMode(CORE, AXIS, INDEX, &s.mode);
    const short r3 = GTN_GetAxisEncPos(CORE, AXIS, &s.encoder);
    const short r4 = GTN_GetPrfPos(CORE, AXIS, &s.planned);
    const short r5 = GTN_GetAxisEncVel(CORE, AXIS, &s.velocity);
    const short r6 = GTN_GetAtlTorque(CORE, AXIS, &s.current);
    LOG("[%s] status=0x%08lX mode=%d enc=%.6f prf=%.6f vel=%.6f current_raw=%d rc=[%d,%d,%d,%d,%d,%d]\n",
        phase, s.sts, s.mode, s.encoder, s.planned, s.velocity, s.current,
        r1, r2, r3, r4, r5, r6);
    if (r1 || r2 || r3 || r4 || r5 || r6 || !std::isfinite(s.encoder) ||
        !std::isfinite(s.planned) || !std::isfinite(s.velocity)) return false;
    if (_kbhit()) {
        const int ch = _getch();
        if (ch == 'q' || ch == 'Q' || ch == 27) {
            LOG("ABORT: keyboard stop.\n"); return false;
        }
    }
    const long faultsOrMotion = 0x2 | 0x10 | 0x20 | 0x40 | 0x100 | 0x400;
    if ((s.sts & faultsOrMotion) || (requireEnabled && !(s.sts & 0x200)) ||
        std::abs(static_cast<int>(s.current)) >= MAX_CURRENT_RAW ||
        std::fabs(s.velocity) >= MAX_VELOCITY_PULSE_MS ||
        (originValid && std::fabs(s.encoder - origin) >= MAX_TRAVEL_PULSE)) {
        LOG("ABORT: status/enable/current/velocity/displacement guard.\n"); return false;
    }
    return true;
}

bool sameSwitch(const TSwitchLoopParameter& a, const TSwitchLoopParameter& b) {
    return a.enable == b.enable && a.mode == b.mode && a.dir == b.dir &&
        a.threshold == b.threshold && a.openLoopDac == b.openLoopDac &&
        a.overTime == b.overTime && a.posOffset == b.posOffset &&
        a.reserve[0] == b.reserve[0] && a.reserve[1] == b.reserve[1];
}

bool executeTest() {
    Sample s;
    if (!sample(s, false, "before_enable") || (s.sts & 0x200) || s.mode != 2) {
        LOG("SKIP: initial axis must be disabled, stationary and in mode 2.\n"); return false;
    }
    origin = s.encoder;
    originValid = true;
    if (std::fabs(s.planned - s.encoder) > MAX_POSITION_ERROR_PULSE) {
        LOG("SKIP: initial position mismatch exceeds 1000 pulses; no auto-zero or position synchronization.\n");
        return false;
    }
    TForceControlPrm prm = {};
    double target = 0;
    if (!ok("GetSwitch(original)", GTN_GetSwitchLoopParameter(CORE, AXIS, &originalSwitch)) ||
        !ok("GetForceControlParameter", GTN_GetForceControlParameter(CORE, AXIS, &prm)) ||
        !ok("GetForceValue(original)", GTN_GetForceValue(CORE, AXIS, INDEX, &target))) return false;
    LOG("Original switch: enable=%d mode=%d dir=%ld threshold=%ld dac=%ld time=%.17g offset=%ld reserve=%d,%d\n",
        originalSwitch.enable, originalSwitch.mode, originalSwitch.dir, originalSwitch.threshold,
        originalSwitch.openLoopDac, originalSwitch.overTime, originalSwitch.posOffset,
        originalSwitch.reserve[0], originalSwitch.reserve[1]);
    LOG("Force config: type=%d target=%.17g outputK=%.17g outputB=%.17g\n",
        prm.controlType, target, prm.controlforceK, prm.controlforceB);
    if (originalSwitch.enable != 0 || originalSwitch.mode != 0 ||
        originalSwitch.reserve[0] || originalSwitch.reserve[1] ||
        !std::isfinite(originalSwitch.overTime) || prm.controlType != 0 ||
        !std::isfinite(target) || target != 0 ||
        !std::isfinite(prm.controlforceK) || prm.controlforceK != 1 || prm.controlforceB != 0) {
        LOG("SKIP: requires inactive switch, open-loop configuration, zero target, outputK=1/outputB=0.\n");
        return false;
    }

    // API exercise 1: inactive parameter round-trip, never arm a transition.
    LOG("\n[API 1] Disabled Set/GetSwitch: change threshold with enable=0, then restore.\n");
    TSwitchLoopParameter probe = originalSwitch, probeRead = {};
    probe.threshold = originalSwitch.threshold == 1 ? 2 : 1;
    switchTouched = true; // Restore even when a setter reports failure.
    const short probeSet = GTN_SetSwitchLoopParameter(CORE, AXIS, &probe);
    const short probeGet = GTN_GetSwitchLoopParameter(CORE, AXIS, &probeRead);
    LOG("Inactive round-trip: threshold %ld -> %ld, set=%d get=%d match=%s\n",
        originalSwitch.threshold, probe.threshold, probeSet, probeGet,
        !probeGet && sameSwitch(probe, probeRead) ? "YES" : "NO");
    TSwitchLoopParameter restoredProbe = {};
    const short restoreSet = GTN_SetSwitchLoopParameter(CORE, AXIS, &originalSwitch);
    const short restoreGet = GTN_GetSwitchLoopParameter(CORE, AXIS, &restoredProbe);
    LOG("Inactive restore: set=%d get=%d match=%s\n", restoreSet, restoreGet,
        !restoreGet && sameSwitch(originalSwitch, restoredProbe) ? "YES" : "NO");
    if (probeSet || probeGet || !sameSwitch(probe, probeRead) || restoreSet || restoreGet ||
        !sameSwitch(originalSwitch, restoredProbe)) return false;
    LOG("PARAMETER ROUND-TRIP PASS (disabled; no transition armed).\n");

    // Flag before call: even a failed enable call requires cleanup.
    enableAttempted = true;
    if (!ok("GTN_AxisOn", GTN_AxisOn(CORE, AXIS))) return false;
    DWORD start = GetTickCount();
    do {
        if (!sample(s, true, "enabled_mode2") || s.mode != 2) return false;
        Sleep(POLL_MS);
    } while (GetTickCount() - start < ENABLE_WAIT_MS);
    const double error = s.planned - s.encoder;
    if (std::fabs(error) > MAX_POSITION_ERROR_PULSE) {
        LOG("ABORT: following error exceeds 1000 pulses before switching.\n"); return false;
    }

    TSwitchLoopParameter entry = {};
    entry.enable = 1;
    entry.mode = 0; // Documented position/speed control -> open-loop force.
    entry.dir = 0;  // Error < threshold. Deliberately true at rest, no contact move.
    entry.threshold = static_cast<long>(std::ceil(error)) + 10;
    entry.openLoopDac = 0;
    entry.overTime = 10; // ms, per demo comments
    entry.posOffset = 0;
    LOG("Arm zero-output entry: error=%.6f dir=0 threshold=%ld overTime=10ms dac=0\n", error, entry.threshold);
    switchTouched = true;
    if (!ok("SetSwitch(entry)", GTN_SetSwitchLoopParameter(CORE, AXIS, &entry))) return false;
    bool entered = false;
    start = GetTickCount();
    do {
        if (!sample(s, true, "waiting_force")) return false;
        TSwitchLoopParameter read = {};
        if (!ok("GetSwitch(entry)", GTN_GetSwitchLoopParameter(CORE, AXIS, &read))) return false;
        LOG("Entry progress: mode=%d switchEnable=%d\n", s.mode, read.enable);
        // Vendor demo completion criterion after successful task submission.
        // An immediately completed task need not expose enable=1 to the host.
        if (read.enable == 0) { entered = true; break; }
        Sleep(POLL_MS);
    } while (GetTickCount() - start < SWITCH_TIMEOUT_MS);
    if (!entered) {
        LOG("INCOMPLETE: entry timeout. No trajectory or nonzero output will be added to force a trigger.\n");
        return false;
    }
    LOG("ENTRY PASS: submitted mode=0 task completed (enable=0); observed loopMode=%d.\n", s.mode);
    LOG("[API 2] GetControlLoopMode is recorded, not used to require entry mode 3 or 4.\n");
    LOG("[API 3] SetForceValue -> GetForceValue: zero DAC target, not measured force.\n");
    for (int step = 1; step <= 3; ++step) {
        LOG("Zero target step %d/3: requested DAC=0\n", step);
        if (!ok("GTN_SetForceValue(0) AFTER ENTRY", GTN_SetForceValue(CORE, AXIS, INDEX, 0))) return false;
        double readTarget = 0;
        if (!ok("GTN_GetForceValue(after)", GTN_GetForceValue(CORE, AXIS, INDEX, &readTarget)) ||
            !std::isfinite(readTarget) || readTarget != 0) return false;
        if (!sample(s, true, "zero_target_readback")) return false;
    }
    LOG("ZERO SETTER PASS: 3 successful zero writes and target readbacks; nonzero response NOT tested.\n");
    // Only a brief observation interval; never hold indefinitely in force mode.
    start = GetTickCount();
    do {
        if (!sample(s, true, "zero_force_observation")) return false;
        Sleep(POLL_MS);
    } while (GetTickCount() - start < 50);

    // Normal exit follows the demo: keep enable on, switch mode=1, then
    // verify position/speed control before removing servo enable in cleanup.
    TSwitchLoopParameter exit = {};
    exit.enable = 1;
    exit.mode = 1;
    exit.posOffset = -10; // Demo value; 10 pulses = 0.001 mm on this platform.
    LOG("Normal exit: KEEP ENABLED, mode=1, posOffset=-10 pulses. No retreat move.\n");
    if (!ok("SetSwitch(exit WHILE ENABLED)", GTN_SetSwitchLoopParameter(CORE, AXIS, &exit))) return false;
    bool returned = false;
    start = GetTickCount();
    do {
        if (!sample(s, true, "returning_mode2_enabled")) return false;
        TSwitchLoopParameter read = {};
        if (!ok("GetSwitch(enabled exit)", GTN_GetSwitchLoopParameter(CORE, AXIS, &read))) return false;
        LOG("Enabled exit progress: mode=%d switchEnable=%d\n", s.mode, read.enable);
        if (s.mode == 2 && read.enable == 0) { returned = true; break; }
        Sleep(POLL_MS);
    } while (GetTickCount() - start < SWITCH_TIMEOUT_MS);
    if (!returned) {
        LOG("FAIL: enabled return to mode 2 timed out; emergency cleanup will disable first.\n");
        return false;
    }
    start = GetTickCount();
    do {
        if (!sample(s, true, "mode2_enabled_observation") || s.mode != 2) return false;
        Sleep(POLL_MS);
    } while (GetTickCount() - start < 50);
    LOG("ENABLED EXIT PASS: mode=1 task completed and mode 2 verified while enabled; now AxisOff.\n");
    return true;
}

bool cleanup() {
    bool success = true;
    // On success the enabled return has already completed in executeTest.
    // On failure, remove enable first; do not force an energized recovery.
    // Never rely on GTN_Stop to terminate force output, and never re-enable.
    if (enableAttempted) {
        const short offRc = GTN_AxisOff(CORE, AXIS);
        success = ok("GTN_AxisOff(cleanup)", offRc) && success;
    }
    long sts = 0;
    const short statusRc = GTN_GetSts(CORE, AXIS, &sts);
    if (!ok("GTN_GetSts(cleanup)", statusRc) || (sts & 0x200)) {
        LOG("ATTENTION: disabled state NOT VERIFIED. Use hardware stop; no mode restoration attempted.\n");
        return false;
    }
    if (!switchTouched) return success;

    // Cancel any pending entry before inspecting/restoring the mode.
    TSwitchLoopParameter cancel = {};
    if (!ok("SetSwitch(cancel pending entry)", GTN_SetSwitchLoopParameter(CORE, AXIS, &cancel))) return false;
    short mode = -1;
    if (!ok("GetMode(cleanup)", GTN_GetControlLoopMode(CORE, AXIS, INDEX, &mode))) return false;
    if (mode != 2) {
        TSwitchLoopParameter exit = {};
        exit.enable = 1;
        exit.mode = 1; // Documented unconditional force -> position/speed control.
        exit.posOffset = 0;
        if (!ok("SetSwitch(exit while disabled)", GTN_SetSwitchLoopParameter(CORE, AXIS, &exit))) return false;
        const DWORD start = GetTickCount();
        bool returned = false;
        do {
            TSwitchLoopParameter sw = {};
            if (!ok("GetMode(exit)", GTN_GetControlLoopMode(CORE, AXIS, INDEX, &mode)) ||
                !ok("GetSwitch(exit)", GTN_GetSwitchLoopParameter(CORE, AXIS, &sw))) break;
            LOG("Cleanup exit progress: mode=%d switchEnable=%d\n", mode, sw.enable);
            if (mode == 2 && sw.enable == 0) { returned = true; break; }
            Sleep(POLL_MS);
        } while (GetTickCount() - start < SWITCH_TIMEOUT_MS);
        if (!returned) {
            TSwitchLoopParameter disabled = {};
            ok("SetSwitch(cancel exit timeout)", GTN_SetSwitchLoopParameter(CORE, AXIS, &disabled));
            LOG("ATTENTION: mode 2 restoration NOT VERIFIED. Keep axis disabled.\n");
            return false;
        }
    }
    TSwitchLoopParameter restored = {};
    const short setRc = GTN_SetSwitchLoopParameter(CORE, AXIS, &originalSwitch);
    const short getRc = GTN_GetSwitchLoopParameter(CORE, AXIS, &restored);
    LOG("Restore original switch: set rc=%d get rc=%d match=%s\n", setRc, getRc,
        !getRc && sameSwitch(originalSwitch, restored) ? "YES" : "NO");
    short finalMode = -1;
    long finalSts = 0;
    const short mr = GTN_GetControlLoopMode(CORE, AXIS, INDEX, &finalMode);
    const short sr = GTN_GetSts(CORE, AXIS, &finalSts);
    LOG("Final: mode rc=%d mode=%d status rc=%d sts=0x%08lX\n", mr, finalMode, sr, finalSts);
    return success && !setRc && !getRc && sameSwitch(originalSwitch, restored) &&
        !mr && finalMode == 2 && !sr && !(finalSts & 0x200);
}
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    if (!SetConsoleCtrlHandler(consoleHandler, TRUE)) return 2;
    wchar_t executable[32768] = {};
    if (!GetModuleFileNameW(nullptr, executable, 32768)) return 2;
    wchar_t* slash = std::wcsrchr(executable, L'\\');
    if (!slash) return 2;
    *slash = 0;
    if (!SetCurrentDirectoryW(executable)) return 2;
    SYSTEMTIME t; GetLocalTime(&t);
    char name[128];
    std::snprintf(name, sizeof(name), "zero_mode_%04u%02u%02u_%02u%02u%02u_%03u.log",
        t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,t.wMilliseconds);
    if (fopen_s(&logFile, name, "w")) return 2;
    LOG("水平电机：自动使能及零输出模式切换测试。Q/Esc 退出；硬件急停保持可用。\n"
        "先验证禁用状态参数写入/读回/恢复，再使能并提交mode=0进入、零目标、mode=1回切。\n"
        "进入按demo检查enable归零，loopMode仅记录；回切额外确认模式2，随后撤使能。\n"
        "软件阈值：位移 %.3f mm，速度 50 pulse/ms（5 mm/s），位置误差 1000 pulse（0.1 mm），电流反馈 %d raw；它们不是硬件限幅。\n",
        MAX_TRAVEL_PULSE / PULSE_PER_MM, MAX_CURRENT_RAW);
    bool opened = ok("GTN_OpenCard(5)", GTN_OpenCard(5));
    bool tested = false, cleaned = false;
    if (opened) {
        long netStatus = 0;
        const short rc = GTN_NetInit(100, const_cast<char*>("Googol.xml"), 60, &netStatus);
        LOG("GTN_NetInit: rc=%d status=%ld\n", rc, netStatus);
        if (!rc) tested = executeTest();
        if (enableAttempted || switchTouched) cleaned = cleanup();
        else cleaned = true;
        cleaned = ok("GTN_Close", GTN_Close()) && cleaned;
    }
    LOG("RESULT: test=%s cleanup=%s overall=%s\nLog: %s\n",
        tested ? "PASS" : "FAIL/INCOMPLETE", cleaned ? "PASS" : "NOT VERIFIED",
        tested && cleaned ? "PASS" : "NOT PASSED", name);
    if (!cleaned && opened) LOG("请保持驱动器禁用，检查退出日志；必要时使用硬件急停。\n");
    std::fclose(logFile);
    return tested && cleaned ? 0 : 1;
}
