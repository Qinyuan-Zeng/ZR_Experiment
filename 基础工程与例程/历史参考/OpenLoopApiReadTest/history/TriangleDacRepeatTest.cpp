// Horizontal motor. Zero timing probe then three 0..100..0 DAC triangles. No trajectory.
// The test condition intentionally triggers at rest; it is NOT contact detection.
#include <cstdio>
#include <cmath>
#include <cwchar>
#include <conio.h>
#include "gvn_force_api_ext.h"

namespace {
const short CORE = 2, AXIS = 1, INDEX = 1;
const double PULSE_PER_MM = 10000.0;
const double MAX_TRAVEL_PULSE = 500.0; // 0.05 mm from initial encoder reading
const double MAX_VELOCITY_PULSE_MS = 50.0; // 5 mm/s
const double MAX_POSITION_ERROR_PULSE = 1000.0; // 0.1 mm
const int MAX_CURRENT_RAW = 400; // Software trip only; NOT a drive current clamp.

const DWORD PULSE_MS = 20; // Host-side requested interval; NOT a hardware timeout.
const int PULSE_CURRENT_TRIP_RAW = 50;
bool strictCurrentGuard = false;
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
        std::abs(static_cast<int>(s.current)) >= (strictCurrentGuard ? PULSE_CURRENT_TRIP_RAW : MAX_CURRENT_RAW) ||
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

// 2 ms is a host target, not a guaranteed device update period.
constexpr double UPDATE_MS = 2.0;
constexpr double SAMPLE_MS = 1.0;
constexpr double MAX_LATENESS_MS = 0.5;
constexpr int TICKS_PER_COMMAND = static_cast<int>(UPDATE_MS / SAMPLE_MS);
static_assert(TICKS_PER_COMMAND * SAMPLE_MS == UPDATE_MS, "Invalid timing ratio");
const int ZERO_UPDATES=100, MAX_UPDATE_ROWS=1024;
LARGE_INTEGER qpcFrequency;
struct UpdateRow {
 int stage,index,repeat,sent;
 double due,start,end,readBegin,readEnd,dac,target;
 double setBegin,currentBegin,currentEnd;
 short setRc,getRc,stsRc,encRc,velRc,currentRc,current;
 long sts;double enc,vel;
};
UpdateRow updateRows[MAX_UPDATE_ROWS];int updateCount=0;
FILE* waveCsv=nullptr;
bool zeroTimingPassed=false,trianglePassed=false;
double waveEpoch=0;
double waveMs(){LARGE_INTEGER t;QueryPerformanceCounter(&t);return 1000.0*double(t.QuadPart)/double(qpcFrequency.QuadPart);}
bool waveCancel(){
 if(InterlockedCompareExchange(&cancelRequested,0,0))return true;
 if(_kbhit()){int c=_getch();if(c=='q'||c=='Q'||c==27)return true;}
 return false;
}
bool updateTelemetry(UpdateRow& r){
 r.readBegin=waveMs()-waveEpoch;
 r.stsRc=GTN_GetSts(CORE,AXIS,&r.sts);
 r.encRc=GTN_GetAxisEncPos(CORE,AXIS,&r.enc);
 r.velRc=GTN_GetAxisEncVel(CORE,AXIS,&r.vel);
 r.currentBegin=waveMs()-waveEpoch;
 r.currentRc=GTN_GetAtlTorque(CORE,AXIS,&r.current);
 r.currentEnd=waveMs()-waveEpoch;
 r.readEnd=waveMs()-waveEpoch;
 return !r.stsRc&&!r.encRc&&!r.velRc&&!r.currentRc&&std::isfinite(r.enc)&&std::isfinite(r.vel)&&
  (r.sts&0x200)&&!(r.sts&(0x2|0x10|0x20|0x40|0x100|0x400))&&
  std::abs(int(r.current))<PULSE_CURRENT_TRIP_RAW&&std::fabs(r.enc-origin)<MAX_TRAVEL_PULSE&&
  std::fabs(r.vel)<MAX_VELOCITY_PULSE_MS&&r.readEnd-r.readBegin<10;
}
bool updateStage(int stage, int commandCount, int repeat = 0) {
    const double base = waveMs();
    const int samples = (commandCount - 1) * TICKS_PER_COMMAND + 1;
    double settleMin = 0, settleMax = 0;
    for (int i = 0; i < samples; ++i) {
        const double due = base + i * SAMPLE_MS;
        while (waveMs() < due) {
            if (waveCancel()) return false;
        }
        if (waveCancel() || updateCount >= MAX_UPDATE_ROWS) return false;

        UpdateRow& row = updateRows[updateCount++];
        row = {};
        row.stage = stage;
        row.repeat = repeat;
        row.index = i;
        row.due = due - waveEpoch;
        row.start = waveMs() - waveEpoch;
        row.setBegin = row.end = row.currentBegin = row.currentEnd = -1;
        row.setRc = row.getRc = row.stsRc = row.encRc =
            row.velRc = row.currentRc = -32768;
        if (row.start - row.due > MAX_LATENESS_MS) return false;

        const int point = i / TICKS_PER_COMMAND;
        row.dac = stage == 1 ? 20.0 * (point <= 5 ? point : 10 - point) : 0.0;
        if (i % TICKS_PER_COMMAND == 0) {
            row.sent = 1;
            row.setBegin = waveMs() - waveEpoch;
            if (row.setBegin - row.due > MAX_LATENESS_MS) {
                row.sent = 0;
                row.setBegin = -1;
                return false;
            }
            row.setRc = GTN_SetForceValue(CORE, AXIS, INDEX, row.dac);
            row.end = waveMs() - waveEpoch;
            if (row.setRc) return false;

            row.getRc = GTN_GetForceValue(CORE, AXIS, INDEX, &row.target);
            if (row.getRc || !std::isfinite(row.target) || row.target != row.dac)
                return false;
        }
        if (!updateTelemetry(row) || waveCancel()) return false;
        if (waveMs() > due + SAMPLE_MS) return false;

        // Before the next trial, verify the tail of the zero-output window.
        if (stage == 2 && i >= samples - 10) {
            if (i == samples - 10) settleMin = settleMax = row.enc;
            if (row.enc < settleMin) settleMin = row.enc;
            if (row.enc > settleMax) settleMax = row.enc;
            if (std::abs(int(row.current)) > 2 || settleMax - settleMin > 10)
                return false;
        }
    }
    return true;
}
bool runWaveTest(){
 updateCount=0;waveEpoch=waveMs();
 bool good=updateStage(0,ZERO_UPDATES);
 zeroTimingPassed=good;
 // No logs/files/allocations while the streaming sequence is active.
 if(good){
  const double until=waveMs()+50;
  while(good&&waveMs()<until){UpdateRow r={};good=updateTelemetry(r)&&!waveCancel();}
 }
 int completed = 0;
 for (int repeat = 1; good && repeat <= 3; ++repeat) {
  good = updateStage(1, 11, repeat);
  if (good) ++completed;
  if (good && repeat < 3) good = updateStage(2, 100, repeat);
 }
 trianglePassed = completed == 3;
 // Always clear, including when a setter reports failure or scheduling overruns.
 const double zeroBegin=waveMs()-waveEpoch;
 const short zeroRc=GTN_SetForceValue(CORE,AXIS,INDEX,0);
 const double zeroEnd=waveMs()-waveEpoch;
 good=good&&!zeroRc;
 if(!good)GTN_AxisOff(CORE,AXIS);
 double target=0;short getRc=-1;
 if(good){getRc=GTN_GetForceValue(CORE,AXIS,INDEX,&target);good=!getRc&&std::isfinite(target)&&target==0;}
 if(good){
  // 49 feedback samples after the final zero.
  good=updateStage(2,25);
 }
 if(!good)GTN_AxisOff(CORE,AXIS);
 LOG("STREAM: zeroTiming=%s triangle=%s finalZero=%d finalGet=%d target=%.17g zeroBegin=%.6f zeroEnd=%.6f ms overall=%s\n",
  zeroTimingPassed?"PASS":"FAIL",trianglePassed?"PASS":"NOT PASSED",zeroRc,getRc,target,zeroBegin,zeroEnd,good?"PASS":"FAIL");
 return good;
}
bool exportWaveRows(){
 double prev[3]={},maxLate[3]={},maxCall[3]={},minDt[3]={1e100,1e100,1e100},maxDt[3]={};int n[3]={};
 for(int i=0;i<updateCount;++i){const UpdateRow&r=updateRows[i];int s=r.stage;
  double dt=r.index!=0?r.start-prev[s]:0;prev[s]=r.start;
  if(r.index!=0&&dt<minDt[s])minDt[s]=dt;if(r.index!=0&&dt>maxDt[s])maxDt[s]=dt;++n[s];
  if(r.start-r.due>maxLate[s])maxLate[s]=r.start-r.due;
  if(r.sent&&r.end-r.setBegin>maxCall[s])maxCall[s]=r.end-r.setBegin;
  if(waveCsv)std::fprintf(waveCsv,"%d,%d,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.17g,%.17g,%d,%d,%ld,%.6f,%.6f,%d,%d,%d,%d,%d,%d,%d,%.6f,%.6f,%.6f\n",r.stage,r.index,r.due,r.start,r.end,r.start-r.due,dt,r.readBegin,r.readEnd,r.dac,r.target,r.setRc,r.getRc,r.sts,r.enc,r.vel,r.current,r.stsRc,r.encRc,r.velRc,r.currentRc,r.repeat,r.sent,r.setBegin,r.currentBegin,r.currentEnd);
 }
 for(int s=0;s<3;++s)if(n[s])LOG("TIMING: stage=%d rows=%d maxLate=%.6fms maxSetCall=%.6fms dtMin=%.6fms dtMax=%.6fms\n",s,n[s],maxLate[s],maxCall[s],minDt[s]<1e100?minDt[s]:0,maxDt[s]);
 return waveCsv&&!std::fflush(waveCsv)&&!std::ferror(waveCsv);
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
    entry.threshold = static_cast<long>(std::ceil(std::fabs(error))) + 10;
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
    strictCurrentGuard=true;
    if(!sample(s,true,"before_stream"))return false;
    if(!runWaveTest())return false;
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

    // A failed zero write may leave a stale nonzero target, even after AxisOff.
    double finalTarget = 0;
    if (!ok("GetForceValue(disabled cleanup)", GTN_GetForceValue(CORE, AXIS, INDEX, &finalTarget)) ||
        !std::isfinite(finalTarget)) return false;
    if (finalTarget != 0) {
        const short zr = GTN_SetForceValue(CORE, AXIS, INDEX, 0);
        const short gr = GTN_GetForceValue(CORE, AXIS, INDEX, &finalTarget);
        LOG("Clear stale target while disabled: set=%d get=%d target=%.17g\n", zr,gr,finalTarget);
        if (zr || gr || !std::isfinite(finalTarget) || finalTarget != 0) {
            LOG("ATTENTION: nonzero target may remain. Keep disabled; do not re-enable.\n");
            return false;
        }
    }

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
    std::snprintf(name, sizeof(name), "triangle_repeat_%04u%02u%02u_%02u%02u%02u_%03u.log",
        t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,t.wMilliseconds);
    if (fopen_s(&logFile, name, "w")) return 2;
    if(!QueryPerformanceFrequency(&qpcFrequency)||qpcFrequency.QuadPart<=0)return 2;
    char csvName[180];std::snprintf(csvName,sizeof(csvName),"%s.csv",name);
    if(fopen_s(&waveCsv,csvName,"w"))return 2;
    std::fprintf(waveCsv,"stage,index,due_ms,tick_begin_ms,set_end_ms,lateness_ms,actual_dt_ms,telemetry_begin_ms,telemetry_end_ms,dac,target_read,set_rc,get_rc,status,encoder_pulse,velocity_pulse_ms,current_raw,status_rc,encoder_rc,velocity_rc,current_rc,repeat,sent,set_call_begin_ms,current_begin_ms,current_end_ms\n");
    if(std::fflush(waveCsv))return 2;
    LOG("自动使能：2ms发DAC、1ms采样；零目标时序通过后做三轮0→100→0三角波。\n"
        "高频循环QPC主动定时，不Sleep(1)、不打印、不写盘；读取不是设备同步快照。\n"
        "迟到超过0.5ms或采样时隙超过1ms即中止；轮间约200ms零目标观察。\n"
        "保护：全程位移0.05mm、速度50pulse/ms；采集电流50raw，启动使能400raw。\n"
        "时序和归零均为PC软件控制，不能保证硬件实时性；Q/Esc/Ctrl+C请求退出。\nCSV: %s\n",csvName);
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
    const bool saved=exportWaveRows();
    cleaned=cleaned&&saved;
    LOG("RESULT: test=%s cleanup=%s overall=%s\nLog: %s\n",
        tested ? "PASS" : "FAIL/INCOMPLETE", cleaned ? "PASS" : "NOT VERIFIED",
        tested && cleaned ? "PASS" : "NOT PASSED", name);
    if (!cleaned && opened) LOG("请保持驱动器禁用，检查退出日志；必要时使用硬件急停。\n");
    std::fclose(waveCsv);
    std::fclose(logFile);
    return tested && cleaned ? 0 : 1;
}
