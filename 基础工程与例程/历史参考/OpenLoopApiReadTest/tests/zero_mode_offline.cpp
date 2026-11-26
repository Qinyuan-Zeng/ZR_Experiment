// No gts.lib linked; all card functions below are fakes.
#define main hardware_main_not_called
#include "../ZeroOutputModeSwitchTest.cpp"
#undef main
#include <cassert>

static bool enabled, refuseEntry, failZero, jumpOnEntry, failDisable, refuseExit;
static bool reportFour, failProbeSet, mismatchProbe, failEntryGet;
static short simulatedMode;
static double simulatedTarget, initialError, fakeVelocity;
static int enableCount, forceWrites, enabledExits;
static short modeAtOff;
static TSwitchLoopParameter simSwitch;
GT_API GTN_OpenCard(short, void*, char*) { return -99; }
GT_API GTN_NetInit(short, void*, short, long*) { return -99; }
GT_API GTN_Close() { return 0; }
GT_API GTN_GetSts(short, short, long* p, short, unsigned long*) { *p = enabled ? 0x200 : 0; return 0; }
GT_API GTN_GetControlLoopMode(short, short, short, short* p) {
    *p = reportFour && simulatedMode == 3 ? 4 : simulatedMode; return 0;
}
GT_API GTN_GetAxisEncPos(short, short, double* p, short, unsigned long*) {
    *p = jumpOnEntry && simulatedMode == 3 ? 2100 : 0; return 0;
}
GT_API GTN_GetPrfPos(short, short, double* p, short, unsigned long*) { *p = initialError; return 0; }
GT_API GTN_GetAxisEncVel(short, short, double* p, short, unsigned long*) { *p = fakeVelocity; return 0; }
GT_API GTN_GetAtlTorque(short, short, short* p) { *p = 0; return 0; }
GT_API GTN_GetSwitchLoopParameter(short, short, TSwitchLoopParameter* p) {
    if (failEntryGet && simulatedMode == 3) return 1;
    *p = simSwitch;
    if (mismatchProbe && !enabled && simSwitch.threshold == 1) p->threshold = 99;
    return 0;
}
GT_API GTN_SetSwitchLoopParameter(short, short, TSwitchLoopParameter* p) {
    assert(p->openLoopDac == 0);
    simSwitch = *p;
    if (failProbeSet && !enabled && p->threshold == 1) return 1;
    if (p->enable && p->mode == 0 && !refuseEntry) { simulatedMode = 3; simSwitch.enable = 0; }
    if (p->enable && p->mode == 1) {
        if (enabled) { ++enabledExits; assert(p->posOffset == -10); }
        if (!refuseExit) { simulatedMode = 2; simSwitch.enable = 0; }
    }
    return 0;
}
GT_API GTN_GetForceControlParameter(short, short, TForceControlPrm* p) { *p = {}; p->controlforceK = 1; return 0; }
GT_API GTN_GetForceValue(short, short, short, double* p) { *p = simulatedTarget; return 0; }
GT_API GTN_SetForceValue(short, short, short, double v) {
    assert(v == 0 && enabled && simulatedMode == 3); ++forceWrites; return failZero ? 1 : 0;
}
GT_API GTN_AxisOn(short, short) { enabled = true; ++enableCount; return 0; }
GT_API GTN_AxisOff(short, short) { modeAtOff = simulatedMode; if (failDisable) return 1; enabled = false; return 0; }

static void resetFake() {
    enabled = refuseEntry = failZero = jumpOnEntry = failDisable = refuseExit = false;
    reportFour = failProbeSet = mismatchProbe = failEntryGet = false;
    simulatedMode = 2; simulatedTarget = initialError = fakeVelocity = 0;
    enableCount = forceWrites = enabledExits = 0; modeAtOff = -1; simSwitch = {};
    cancelRequested = 0;
    origin = 0; originValid = enableAttempted = switchTouched = false; originalSwitch = {};
}
int main() {
    resetFake(); assert(executeTest()); assert(cleanup());
    assert(!enabled && simulatedMode == 2 && forceWrites == 3 && simSwitch.enable == 0);
    assert(enabledExits == 1 && modeAtOff == 2);
    resetFake(); initialError = 1001; assert(!executeTest() && enableCount == 0);
    resetFake(); initialError = 1000; assert(executeTest()); assert(cleanup());
    resetFake(); fakeVelocity = 50; assert(!executeTest() && enableCount == 0);
    resetFake(); simulatedTarget = 1; assert(!executeTest() && enableCount == 0);
    resetFake(); enabled = true; assert(!executeTest() && enableCount == 0);
    resetFake(); refuseEntry = true; assert(!executeTest()); assert(cleanup());
    assert(!enabled && simulatedMode == 2 && forceWrites == 0 && simSwitch.enable == 0);
    resetFake(); failZero = true; assert(!executeTest()); assert(cleanup());
    assert(!enabled && simulatedMode == 2 && modeAtOff == 3 && enabledExits == 0);
    resetFake(); jumpOnEntry = true; assert(!executeTest()); assert(cleanup());
    assert(!enabled && simulatedMode == 2 && forceWrites == 0);
    resetFake(); assert(executeTest()); failDisable = true;
    assert(!cleanup()); assert(enabled && simulatedMode == 2);
    resetFake(); refuseExit = true; assert(!executeTest()); assert(!cleanup());
    assert(!enabled && simulatedMode == 3 && enabledExits == 1 && simSwitch.enable == 0);
    resetFake(); reportFour = true; assert(executeTest()); assert(cleanup());
    assert(!enabled && simulatedMode == 2 && forceWrites == 3);
    resetFake(); failProbeSet = true; assert(!executeTest()); assert(cleanup());
    assert(enableCount == 0 && simSwitch.threshold == 0);
    resetFake(); mismatchProbe = true; assert(!executeTest()); assert(cleanup());
    assert(enableCount == 0 && simSwitch.threshold == 0);
    resetFake(); failEntryGet = true; assert(!executeTest()); assert(cleanup());
    assert(!enabled && forceWrites == 0);
    std::puts("ZERO-MODE OFFLINE CHECKS PASSED: 15 cases, fake API only.");
}
