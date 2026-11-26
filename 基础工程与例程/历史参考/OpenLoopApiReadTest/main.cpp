#include <cstdio>
#include <cstddef>
#include <cwchar>
#include <string>
#include <cmath>
#include "gvn_force_api_ext.h"

static_assert(offsetof(TForceControlPrm, feedbackForceK) == 8, "ABI mismatch");
static_assert(offsetof(TForceControlPrm, outputReverse) == 48, "ABI mismatch");

static FILE* logFile = nullptr;
#define REPORT(...) do { std::printf(__VA_ARGS__); if (logFile) { \
    std::fprintf(logFile, __VA_ARGS__); std::fflush(logFile); } } while (0)

static bool sameSwitch(const TSwitchLoopParameter& a, const TSwitchLoopParameter& b) {
    // Compare fields, never struct padding.
    return a.enable == b.enable && a.mode == b.mode && a.dir == b.dir &&
        a.threshold == b.threshold && a.openLoopDac == b.openLoopDac &&
        a.overTime == b.overTime && a.posOffset == b.posOffset &&
        a.reserve[0] == b.reserve[0] && a.reserve[1] == b.reserve[1];
}

static bool readyForWrite(short core, short axis, short index,
                          TSwitchLoopParameter* snapshot) {
    long sts = 0;
    short mode = -1;
    TSwitchLoopParameter sw = {};
    short r1 = GTN_GetSts(core, axis, &sts);
    short r2 = GTN_GetControlLoopMode(core, axis, index, &mode);
    short r3 = GTN_GetSwitchLoopParameter(core, axis, &sw);
    REPORT("Precheck: GetSts rc=%d sts=0x%08lX; GetMode rc=%d mode=%d; GetSwitch rc=%d enable=%d\n",
           r1, sts, r2, mode, r3, sw.enable);
    // Exclude enabled axes, motion, drive alarms, following errors,
    // limits and emergency-stop indications. Any uncertainty blocks writes.
    const long blocked = 0x2 | 0x10 | 0x20 | 0x40 | 0x100 | 0x200 | 0x400;
    if (r1 || r2 || r3 || (sts & blocked) || mode != 2 || sw.enable != 0 ||
        sw.mode != 0 || sw.reserve[0] || sw.reserve[1] || !std::isfinite(sw.overTime)) {
        REPORT("SKIP: write preconditions not met; no write performed.\n");
        return false;
    }
    if (snapshot) *snapshot = sw;
    return true;
}

// Result: 0 = verified, 1 = failed, 2 = skipped. No motion APIs are used.
static int runWriteChecks(short core, short axis, short index) {
    REPORT("\n--- Disabled-axis write/readback checks ---\n");
    TSwitchLoopParameter original = {};
    if (!readyForWrite(core, axis, index, &original)) return 2;

    TSwitchLoopParameter test = original;
    test.threshold = original.threshold == 1 ? 2 : 1;
    // enable stays zero; do not alter mode, DAC, offset, direction or timing.
    REPORT("Switch test: threshold %ld -> %ld; enable remains 0.\n",
           original.threshold, test.threshold);
    const short setRc = GTN_SetSwitchLoopParameter(core, axis, &test);
    REPORT("GTN_SetSwitchLoopParameter(test): rc=%d\n", setRc);
    TSwitchLoopParameter observed = {};
    const short getRc = GTN_GetSwitchLoopParameter(core, axis, &observed);
    const bool matched = getRc == 0 && sameSwitch(test, observed);
    REPORT("GTN_GetSwitchLoopParameter(test): rc=%d; readback=%s\n",
           getRc, matched ? "MATCH" : "MISMATCH/ERROR");

    // Always attempt restoration after the first setter, even if it reports
    // failure: a failed call alone does not prove no parameter was changed.
    const short restoreRc = GTN_SetSwitchLoopParameter(core, axis, &original);
    TSwitchLoopParameter restored = {};
    const short restoreGetRc = GTN_GetSwitchLoopParameter(core, axis, &restored);
    const bool restoreMatch = restoreGetRc == 0 && sameSwitch(original, restored);
    REPORT("GTN_SetSwitchLoopParameter(restore): rc=%d\n", restoreRc);
    REPORT("GTN_GetSwitchLoopParameter(restore): rc=%d; restored=%s\n",
           restoreGetRc, restoreMatch ? "YES" : "NOT VERIFIED");
    REPORT("Switch setter/readback: %s\n",
           !setRc && matched && !restoreRc && restoreMatch ? "PASS" : "FAIL");
    if (restoreRc || !restoreMatch) {
        REPORT("ATTENTION: original switch parameters could not be verified restored.\n"
               "No further tests. Do not enable the axis; inspect the log/configuration.\n");
        return 1;
    }
    if (setRc || !matched) return 1;

    // Recheck state before a second independent setter. Only repeat zero:
    // never introduce a nonzero force command or overwrite a nonzero target.
    if (!readyForWrite(core, axis, index, nullptr)) return 2;
    double target = 0;
    TForceControlPrm prm = {};
    const short targetRc = GTN_GetForceValue(core, axis, index, &target);
    const short prmRc = GTN_GetForceControlParameter(core, axis, &prm);
    REPORT("Zero-target precheck: GetForceValue rc=%d target=%.17g; GetForceControlParameter rc=%d controlType=%d\n",
           targetRc, target, prmRc, prm.controlType);
    if (targetRc || prmRc || !std::isfinite(target) || target != 0 || prm.controlType != 0) {
        REPORT("SKIP zero-target setter: requires successful reads, open-loop type, original target exactly zero.\n");
        return 2;
    }
    const short forceSetRc = GTN_SetForceValue(core, axis, index, 0.0);
    double after = 0;
    const short forceGetRc = GTN_GetForceValue(core, axis, index, &after);
    const bool zeroMatch = forceGetRc == 0 && std::isfinite(after) && after == 0;
    REPORT("GTN_SetForceValue(0): rc=%d\n", forceSetRc);
    REPORT("GTN_GetForceValue(after zero write): rc=%d target=%.17g\n", forceGetRc, after);
    REPORT("Zero-target setter: %s (zero-only call check; nonzero output NOT tested).\n",
           !forceSetRc && zeroMatch ? "PASS" : "FAIL");
    return !forceSetRc && zeroMatch ? 0 : 1;
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    // Use the executable directory for XML lookup and logs, independent of VS settings.
    wchar_t executable[32768] = {};
    if (!GetModuleFileNameW(nullptr, executable, 32768)) return 2;
    wchar_t* slash = std::wcsrchr(executable, L'\\');
    if (!slash) return 2;
    *slash = L'\0';
    if (!SetCurrentDirectoryW(executable)) return 2;
    std::puts("GVN 开环力控接口 读取及禁用状态写入测试\n"
              "自动使用 force_control_platform_test 的连接配置，无需输入。\n"
              "不使能、不运动、不切换环路、不写非零力目标。\n"
              "自动检查未使能及运控模式，再测试禁用切换参数的写入、读回和恢复。\n");
    // Fixed connection settings from the working platform project.
    const short core = 2;
    const short axis = 1;
    const short index = 1; // Test PID index, retained from the force demo.
    const short channel = 5;
    const short initNetwork = 1;
    const std::string xml = "Googol.xml";
    const short timeout = 60;
    SYSTEMTIME now;
    GetLocalTime(&now);
    char filename[160];
    std::snprintf(filename, sizeof(filename),
        "api_test_%04u%02u%02u_%02u%02u%02u_%03u.log", now.wYear,
        now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
    if (fopen_s(&logFile, filename, "w") != 0) {
        std::puts("Cannot create log in current directory; no card calls made.");
        return 2;
    }
    char dllPath[MAX_PATH] = {};
    GetModuleFileNameA(GetModuleHandleA("gts.dll"), dllPath, MAX_PATH);
    REPORT("DLL: %s\nProcess bits: %zu\nCORE=%d AXIS=%d INDEX=%d\n",
           dllPath, sizeof(void*) * 8, core, axis, index);
    REPORT("Network initialization: %d; XML=%s; overTime=%d\n",
           initNetwork, xml.c_str(), timeout);

    short rc = GTN_OpenCard(channel);
    REPORT("GTN_OpenCard(channel=%d): rc=%d\n", channel, rc);
    if (rc != 0) {
        REPORT("Open failed. No further calls. Consult the matching error-code manual.\n");
        std::fclose(logFile);
        return 1;
    }

    if (initNetwork) {
        long netStatus = 0;
        rc = GTN_NetInit(100, const_cast<char*>(xml.c_str()), timeout, &netStatus);
        REPORT("GTN_NetInit(mode=100): rc=%d status=%ld\n", rc, netStatus);
        if (rc != 0) {
            REPORT("Network initialization failed; target queries skipped.\n");
            const short closeRc = GTN_Close();
            REPORT("GTN_Close: rc=%d\nLog: %s\n", closeRc, filename);
            std::fclose(logFile);
            return 1;
        }
    }

    int failures = 0;
    char* version = nullptr;
    rc = GTN_GetVersion(core, &version);
    REPORT("GTN_GetVersion: rc=%d\n", rc);
    if (rc == 0 && version) REPORT("  version=%s\n", version);
    // A version query is diagnostic; assess the four target getters separately.

    short mode = -1;
    rc = GTN_GetControlLoopMode(core, axis, index, &mode);
    REPORT("GTN_GetControlLoopMode: rc=%d\n", rc);
    if (rc == 0) REPORT("  loopMode=%d (raw value; do not infer open/closed force type alone)\n", mode);
    else ++failures;

    TSwitchLoopParameter sw = {};
    rc = GTN_GetSwitchLoopParameter(core, axis, &sw);
    REPORT("GTN_GetSwitchLoopParameter: rc=%d\n", rc);
    if (rc == 0) {
        REPORT("  enable=%d mode=%d dir=%ld threshold=%ld openLoopDac=%ld\n"
               "  overTime=%.17g posOffset=%ld reserve=[%d,%d]\n",
               sw.enable, sw.mode, sw.dir, sw.threshold, sw.openLoopDac,
               sw.overTime, sw.posOffset, sw.reserve[0], sw.reserve[1]);
    } else ++failures;

    TForceControlPrm prm = {};
    rc = GTN_GetForceControlParameter(core, axis, &prm);
    REPORT("GTN_GetForceControlParameter: rc=%d\n", rc);
    if (rc == 0) {
        REPORT("  controlType=%d feedbackType=%d feedbackSrcIndex=%d\n"
               "  feedbackForceK=%.17g feedbackForceB=%.17g\n"
               "  controlforceK=%.17g controlforceB=%.17g outputLimit=%.17g\n"
               "  outputReverse=%d calibrationMode=%d\n",
               prm.controlType, prm.feedbackType, prm.feedbackSrcIndex,
               prm.feedbackForceK, prm.feedbackForceB, prm.controlforceK,
               prm.controlforceB, prm.outputLimit, prm.outputReverse, prm.calibrationMode);
    } else ++failures;

    double target = 0;
    rc = GTN_GetForceValue(core, axis, index, &target);
    REPORT("GTN_GetForceValue: rc=%d\n", rc);
    if (rc == 0) REPORT("  configured target=%.17g (not measured force; DAC bits in open-loop)\n", target);
    else ++failures;

    int writeResult = 2;
    if (failures == 0) writeResult = runWriteChecks(core, axis, index);
    else REPORT("SKIP all writes: one or more initial getters failed.\n");

    rc = GTN_Close();
    REPORT("GTN_Close: rc=%d\n", rc);
    REPORT("Target getters: %d/4 successful. Close status is separate.\n"
           "This does not verify motion, force accuracy, or switching.\nLog: %s\n",
           4 - failures, filename);
    REPORT("Write checks overall: %s\n", writeResult == 0 ? "PASS" : writeResult == 1 ? "FAIL" : "SKIPPED/INCOMPLETE");
    std::fclose(logFile);
    return (failures || rc || writeResult == 1) ? 1 : writeResult == 2 ? 2 : 0;
}
